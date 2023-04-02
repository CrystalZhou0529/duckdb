#include "duckdb/planner/binder.hpp"
#include "duckdb/parser/tableref/pivotref.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/expression/case_expression.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/comparison_expression.hpp"
#include "duckdb/parser/expression/conjunction_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/planner/query_node/bound_select_node.hpp"
#include "duckdb/parser/expression/star_expression.hpp"
#include "duckdb/common/types/value_map.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"
#include "duckdb/parser/expression/operator_expression.hpp"
#include "duckdb/planner/tableref/bound_subqueryref.hpp"

namespace duckdb {

struct PivotValueElement {
	vector<Value> values;
	string name;
};

static void ConstructPivots(PivotRef &ref, vector<PivotValueElement> &pivot_values, idx_t pivot_idx = 0,
                            PivotValueElement current_value = PivotValueElement()) {
	auto &pivot = ref.pivots[pivot_idx];
	bool last_pivot = pivot_idx + 1 == ref.pivots.size();
	for (auto &entry : pivot.entries) {
		PivotValueElement new_value = current_value;
		string name = entry.alias;
		D_ASSERT(entry.values.size() == pivot.pivot_expressions.size());
		for (idx_t v = 0; v < entry.values.size(); v++) {
			auto &value = entry.values[v];
			new_value.values.push_back(value);
			if (entry.alias.empty()) {
				if (name.empty()) {
					name = value.ToString();
				} else {
					name += "_" + value.ToString();
				}
			}
		}
		if (!current_value.name.empty()) {
			new_value.name = current_value.name + "_" + name;
		} else {
			new_value.name = std::move(name);
		}
		if (last_pivot) {
			pivot_values.push_back(std::move(new_value));
		} else {
			// need to recurse
			ConstructPivots(ref, pivot_values, pivot_idx + 1, std::move(new_value));
		}
	}
}

static void ExtractPivotExpressions(ParsedExpression &expr, case_insensitive_set_t &handled_columns) {
	if (expr.type == ExpressionType::COLUMN_REF) {
		auto &child_colref = (ColumnRefExpression &)expr;
		if (child_colref.IsQualified()) {
			throw BinderException("PIVOT expression cannot contain qualified columns");
		}
		handled_columns.insert(child_colref.GetColumnName());
	}
	ParsedExpressionIterator::EnumerateChildren(
	    expr, [&](ParsedExpression &child) { ExtractPivotExpressions(child, handled_columns); });
}

struct PivotBindState {
	vector<string> internal_group_names;
	vector<string> group_names;
	vector<string> aggregate_names;
	vector<string> internal_aggregate_names;
	vector<string> internal_pivot_names;
	vector<string> internal_map_names;
};

static unique_ptr<SelectNode> PivotStageOne(PivotBindState &bind_state, PivotRef &ref, vector<unique_ptr<ParsedExpression>> all_columns, const case_insensitive_set_t &handled_columns) {
	auto subquery_stage1 = make_unique<SelectNode>();
	subquery_stage1->from_table = std::move(ref.source);
	if (ref.groups.empty()) {
		// if rows are not specified any columns that are not pivoted/aggregated on are added to the GROUP BY clause
		for (auto &entry : all_columns) {
			if (entry->type != ExpressionType::COLUMN_REF) {
				throw InternalException("Unexpected child of pivot source - not a ColumnRef");
			}
			auto &columnref = (ColumnRefExpression &)*entry;
			if (handled_columns.find(columnref.GetColumnName()) == handled_columns.end()) {
				// not handled - add to grouping set
				subquery_stage1->groups.group_expressions.push_back(
				    make_unique<ConstantExpression>(Value::INTEGER(subquery_stage1->select_list.size() + 1)));
				subquery_stage1->select_list.push_back(std::move(entry));
			}
		}
	} else {
		// if rows are specified only the columns mentioned in rows are added as groups
		for (auto &row : ref.groups) {
			subquery_stage1->groups.group_expressions.push_back(
			    make_unique<ConstantExpression>(Value::INTEGER(subquery_stage1->select_list.size() + 1)));
			subquery_stage1->select_list.push_back(make_unique<ColumnRefExpression>(row));
		}
	}
	idx_t group_count = 0;
	for (auto &expr : subquery_stage1->select_list) {
		bind_state.group_names.push_back(expr->GetName());
		if (expr->alias.empty()) {
			expr->alias = "__internal_pivot_group" + std::to_string(++group_count);
		}
		bind_state.internal_group_names.push_back(expr->alias);
	}
	// group by all of the pivot values
	idx_t pivot_count = 0;
	for (auto &pivot_column : ref.pivots) {
		for (auto &pivot_expr : pivot_column.pivot_expressions) {
			if (pivot_expr->alias.empty()) {
				pivot_expr->alias = "__internal_pivot_ref" + std::to_string(++pivot_count);
			}
			auto pivot_alias = pivot_expr->alias;
			subquery_stage1->groups.group_expressions.push_back(
			    make_unique<ConstantExpression>(Value::INTEGER(subquery_stage1->select_list.size() + 1)));
			subquery_stage1->select_list.push_back(std::move(pivot_expr));
			pivot_expr = make_unique<ColumnRefExpression>(std::move(pivot_alias));
		}
	}
	idx_t aggregate_count = 0;
	// finally add the aggregates
	for (auto &aggregate : ref.aggregates) {
		auto aggregate_alias = "__internal_pivot_aggregate" + std::to_string(++aggregate_count);
		bind_state.aggregate_names.push_back(aggregate->alias);
		bind_state.internal_aggregate_names.push_back(aggregate_alias);
		aggregate->alias = std::move(aggregate_alias);
		subquery_stage1->select_list.push_back(std::move(aggregate));
	}
	return subquery_stage1;
}

static unique_ptr<SelectNode> PivotStageTwo(PivotBindState &bind_state, PivotRef &ref, unique_ptr<SelectNode> subquery_stage1) {
	auto subquery_stage2 = make_unique<SelectNode>();
	// wrap the subquery of stage 1
	auto subquery_select = make_unique<SelectStatement>();
	subquery_select->node = std::move(subquery_stage1);
	auto subquery_ref = make_unique<SubqueryRef>(std::move(subquery_select));

	// add all of the groups
	for (idx_t gr = 0; gr < bind_state.internal_group_names.size(); gr++) {
		subquery_stage2->groups.group_expressions.push_back(
		    make_unique<ConstantExpression>(Value::INTEGER(subquery_stage2->select_list.size() + 1)));
		auto group_reference = make_unique<ColumnRefExpression>(bind_state.internal_group_names[gr]);
		group_reference->alias = bind_state.internal_group_names[gr];
		subquery_stage2->select_list.push_back(std::move(group_reference));
	}

	// construct the list aggregates
	for(idx_t aggr = 0; aggr < bind_state.internal_aggregate_names.size(); aggr++) {
		auto colref = make_unique<ColumnRefExpression>(bind_state.internal_aggregate_names[aggr]);
		vector<unique_ptr<ParsedExpression>> list_children;
		list_children.push_back(std::move(colref));
		auto aggregate = make_unique<FunctionExpression>("list", std::move(list_children));
		aggregate->alias = bind_state.internal_aggregate_names[aggr];
		subquery_stage2->select_list.push_back(std::move(aggregate));
	}
	// FIXME: we should have one list for all of these (as a concatenation of strings maybe?)
	idx_t pivot_count = 0;
	for (auto &pivot : ref.pivots) {
		for (auto &pivot_expr : pivot.pivot_expressions) {
			auto pivot_name = "__internal_pivot_name" + to_string(++pivot_count);
			vector<unique_ptr<ParsedExpression>> list_children;
			list_children.push_back(std::move(pivot_expr));
			auto aggregate = make_unique<FunctionExpression>("list", std::move(list_children));
			aggregate->alias = pivot_name;
			subquery_stage2->select_list.push_back(std::move(aggregate));
			bind_state.internal_pivot_names.push_back(std::move(pivot_name));
		}
	}
	subquery_stage2->from_table = std::move(subquery_ref);
	return subquery_stage2;
}

static unique_ptr<SelectNode> PivotStageThree(PivotBindState &bind_state, PivotRef &ref, unique_ptr<SelectNode> subquery_stage2) {
	auto subquery_stage3 = make_unique<SelectNode>();
	// wrap the subquery of stage 1
	auto subquery_select = make_unique<SelectStatement>();
	subquery_select->node = std::move(subquery_stage2);
	auto subquery_ref = make_unique<SubqueryRef>(std::move(subquery_select));

	// add all of the groups
	for (idx_t gr = 0; gr < bind_state.internal_group_names.size(); gr++) {
		auto group_reference = make_unique<ColumnRefExpression>(bind_state.internal_group_names[gr]);
		group_reference->alias = bind_state.internal_group_names[gr];
		subquery_stage3->select_list.push_back(std::move(group_reference));
	}

	// construct the MAPs
	D_ASSERT(bind_state.internal_pivot_names.size() == bind_state.internal_aggregate_names.size());
	for(idx_t i = 0; i < bind_state.internal_pivot_names.size(); i++) {
		auto map_name = "__internal_pivot_map" + to_string(i + 1);
		vector<unique_ptr<ParsedExpression>> map_children;
		map_children.push_back(make_unique<ColumnRefExpression>(bind_state.internal_pivot_names[i]));
		map_children.push_back(make_unique<ColumnRefExpression>(bind_state.internal_aggregate_names[i]));
		auto function = make_unique<FunctionExpression>("map", std::move(map_children));
		function->alias = map_name;
		bind_state.internal_map_names.push_back(std::move(map_name));
		subquery_stage3->select_list.push_back(std::move(function));
	}
	subquery_stage3->from_table = std::move(subquery_ref);
	return subquery_stage3;
}

static unique_ptr<SelectNode> PivotStageFour(PivotBindState &bind_state, PivotRef &ref, unique_ptr<SelectNode> subquery_stage3, vector<PivotValueElement> pivot_values) {
	auto subquery_stage4 = make_unique<SelectNode>();
	// wrap the subquery of stage 1
	auto subquery_select = make_unique<SelectStatement>();
	subquery_select->node = std::move(subquery_stage3);
	auto subquery_ref = make_unique<SubqueryRef>(std::move(subquery_select));

	// add all of the groups
	for (idx_t gr = 0; gr < bind_state.internal_group_names.size(); gr++) {
		auto group_reference = make_unique<ColumnRefExpression>(bind_state.internal_group_names[gr]);
		group_reference->alias = bind_state.group_names[gr];
		subquery_stage4->select_list.push_back(std::move(group_reference));
	}

	// construct the map extract calls
	for(auto &pivot_value : pivot_values) {
		// FIXME
		if (pivot_value.values.size() != 1) {
			throw InternalException("FIXME multiple pivots");
		}
		for(auto &internal_map : bind_state.internal_map_names) {
			vector<unique_ptr<ParsedExpression>> map_children;
			map_children.push_back(make_unique<ColumnRefExpression>(internal_map));
			map_children.push_back(make_unique<ConstantExpression>(std::move(pivot_value.values[0])));
			auto function = make_unique<FunctionExpression>("map_extract", std::move(map_children));
			vector<unique_ptr<ParsedExpression>> array_children;
			array_children.push_back(std::move(function));
			array_children.push_back(make_unique<ConstantExpression>(Value::INTEGER(1)));
			function = make_unique<FunctionExpression>("array_extract", std::move(array_children));
			function->alias = std::move(pivot_value.name);
			subquery_stage4->select_list.push_back(std::move(function));
		}
	}
	subquery_stage4->from_table = std::move(subquery_ref);
	return subquery_stage4;
}

unique_ptr<SelectNode> Binder::BindPivot(PivotRef &ref, vector<unique_ptr<ParsedExpression>> all_columns) {
	const static idx_t PIVOT_EXPRESSION_LIMIT = 10000;
	// keep track of the columns by which we pivot/aggregate
	// any columns which are not pivoted/aggregated on are added to the GROUP BY clause
	case_insensitive_set_t handled_columns;
	// parse the aggregate, and extract the referenced columns from the aggregate
	for (auto &aggr : ref.aggregates) {
		if (aggr->type != ExpressionType::FUNCTION) {
			throw BinderException(FormatError(*aggr, "Pivot expression must be an aggregate"));
		}
		if (aggr->HasSubquery()) {
			throw BinderException(FormatError(*aggr, "Pivot expression cannot contain subqueries"));
		}
		if (aggr->IsWindow()) {
			throw BinderException(FormatError(*aggr, "Pivot expression cannot contain window functions"));
		}
		ExtractPivotExpressions(*aggr, handled_columns);
	}
	value_set_t pivots;

	// first add all pivots to the set of handled columns, and check for duplicates
	idx_t total_pivots = 1;
	for (auto &pivot : ref.pivots) {
		if (!pivot.pivot_enum.empty()) {
			auto type = Catalog::GetType(context, INVALID_CATALOG, INVALID_SCHEMA, pivot.pivot_enum);
			if (type.id() != LogicalTypeId::ENUM) {
				throw BinderException(
				    FormatError(ref, StringUtil::Format("Pivot must reference an ENUM type: \"%s\" is of type \"%s\"",
				                                        pivot.pivot_enum, type.ToString())));
			}
			auto enum_size = EnumType::GetSize(type);
			for (idx_t i = 0; i < enum_size; i++) {
				auto enum_value = EnumType::GetValue(Value::ENUM(i, type));
				PivotColumnEntry entry;
				entry.values.emplace_back(enum_value);
				entry.alias = std::move(enum_value);
				pivot.entries.push_back(std::move(entry));
			}
		}
		total_pivots *= pivot.entries.size();
		// add the pivoted column to the columns that have been handled
		for (auto &pivot_name : pivot.pivot_expressions) {
			ExtractPivotExpressions(*pivot_name, handled_columns);
		}
		value_set_t pivots;
		for (auto &entry : pivot.entries) {
			D_ASSERT(!entry.star_expr);
			Value val;
			if (entry.values.size() == 1) {
				val = entry.values[0];
			} else {
				val = Value::LIST(LogicalType::VARCHAR, entry.values);
			}
			if (pivots.find(val) != pivots.end()) {
				throw BinderException(FormatError(
				    ref, StringUtil::Format("The value \"%s\" was specified multiple times in the IN clause",
				                            val.ToString())));
			}
			if (entry.values.size() != pivot.pivot_expressions.size()) {
				throw ParserException("PIVOT IN list - inconsistent amount of rows - expected %d but got %d",
				                      pivot.pivot_expressions.size(), entry.values.size());
			}
			pivots.insert(val);
		}
	}
	if (total_pivots >= PIVOT_EXPRESSION_LIMIT) {
		throw BinderException("Pivot column limit of %llu exceeded", PIVOT_EXPRESSION_LIMIT);
	}

	// construct the required pivot values recursively
	vector<PivotValueElement> pivot_values;
	ConstructPivots(ref, pivot_values);

	// pivots have three components
	// - the pivots (i.e. future column names)
	// - the groups (i.e. the future row names
	// - the aggregates (i.e. the values of the pivot columns)

	// executing a pivot statement happens in four stages
	// 1) execute the query "SELECT {groups}, {pivots}, {aggregates} FROM {from_clause} GROUP BY {groups}, {pivots}
	// this computes all values that are required in the final result, but not yet in the correct orientation
	// 2) execute the query "SELECT {groups}, LIST({pivots}), LIST({aggregates}) FROM [Q1] GROUP BY {groups}
	// this pushes all pivots and aggregates that belong to a specific group together in an aligned manner
	// 3) execute the query "SELECT {groups}, MAP(pivot_list, aggregate_list) AS m FROM [Q2]
	// this constructs a MAP vector that we will use to lookup the final value for each pivoted element
	// 4) execute the query "SELECT {groups}, m[pivot_val1] AS pivot_val1, m[pivot_val2] AS pivot_val2, m[pivot_val3], ... FROM [Q3]
	// this constructs the fully pivoted final result

	PivotBindState bind_state;
	// Pivot Stage 1
	// SELECT {groups}, {pivots}, {aggregates} FROM {from_clause} GROUP BY {groups}, {pivots}
	auto subquery_stage1 = PivotStageOne(bind_state, ref, std::move(all_columns), handled_columns);

	// Pivot stage 2
	// SELECT {groups}, LIST({pivots}), LIST({aggregates}) FROM [Q1] GROUP BY {groups}
	auto subquery_stage2 = PivotStageTwo(bind_state, ref, std::move(subquery_stage1));

	// FIXME: 
	// Pivot stage 3
	// SELECT {groups}, MAP(pivot_list, aggregate_list) AS m FROM [Q2]
	auto subquery_stage3 = PivotStageThree(bind_state, ref, std::move(subquery_stage2));

	// Pivot stage 4
	// SELECT {groups}, m[pivot_val1] AS pivot_val1, m[pivot_val2] AS pivot_val2, m[pivot_val3], ... FROM [Q3]
	auto subquery_stage4 = PivotStageFour(bind_state, ref, std::move(subquery_stage3), std::move(pivot_values));

	return subquery_stage4;
}

unique_ptr<SelectNode> Binder::BindUnpivot(Binder &child_binder, PivotRef &ref,
                                           vector<unique_ptr<ParsedExpression>> all_columns,
                                           unique_ptr<ParsedExpression> &where_clause) {
	D_ASSERT(ref.groups.empty());
	D_ASSERT(ref.pivots.size() == 1);

	unique_ptr<ParsedExpression> expr;
	auto select_node = make_unique<SelectNode>();
	select_node->from_table = std::move(ref.source);

	// handle the pivot
	auto &unpivot = ref.pivots[0];

	// handle star expressions in any entries
	vector<PivotColumnEntry> new_entries;
	for (auto &entry : unpivot.entries) {
		if (entry.star_expr) {
			D_ASSERT(entry.values.empty());
			vector<unique_ptr<ParsedExpression>> star_columns;
			child_binder.ExpandStarExpression(std::move(entry.star_expr), star_columns);

			for (auto &col : star_columns) {
				if (col->type != ExpressionType::COLUMN_REF) {
					throw InternalException("Unexpected child of unpivot star - not a ColumnRef");
				}
				auto &columnref = (ColumnRefExpression &)*col;
				PivotColumnEntry new_entry;
				new_entry.values.emplace_back(columnref.GetColumnName());
				new_entry.alias = columnref.GetColumnName();
				new_entries.push_back(std::move(new_entry));
			}
		} else {
			new_entries.push_back(std::move(entry));
		}
	}
	unpivot.entries = std::move(new_entries);

	case_insensitive_set_t handled_columns;
	case_insensitive_map_t<string> name_map;
	for (auto &entry : unpivot.entries) {
		for (auto &value : entry.values) {
			handled_columns.insert(value.ToString());
		}
	}

	for (auto &col_expr : all_columns) {
		if (col_expr->type != ExpressionType::COLUMN_REF) {
			throw InternalException("Unexpected child of pivot source - not a ColumnRef");
		}
		auto &columnref = (ColumnRefExpression &)*col_expr;
		auto &column_name = columnref.GetColumnName();
		auto entry = handled_columns.find(column_name);
		if (entry == handled_columns.end()) {
			// not handled - add to the set of regularly selected columns
			select_node->select_list.push_back(std::move(col_expr));
		} else {
			name_map[column_name] = column_name;
			handled_columns.erase(entry);
		}
	}
	if (!handled_columns.empty()) {
		for (auto &entry : handled_columns) {
			throw BinderException("Column \"%s\" referenced in UNPIVOT but no matching entry was found in the table",
			                      entry);
		}
	}
	vector<Value> unpivot_names;
	for (auto &entry : unpivot.entries) {
		string generated_name;
		for (auto &val : entry.values) {
			auto name_entry = name_map.find(val.ToString());
			if (name_entry == name_map.end()) {
				throw InternalException("Unpivot - could not find column name in name map");
			}
			if (!generated_name.empty()) {
				generated_name += "_";
			}
			generated_name += name_entry->second;
		}
		unpivot_names.emplace_back(!entry.alias.empty() ? entry.alias : generated_name);
	}
	vector<vector<unique_ptr<ParsedExpression>>> unpivot_expressions;
	for (idx_t v_idx = 0; v_idx < unpivot.entries[0].values.size(); v_idx++) {
		vector<unique_ptr<ParsedExpression>> expressions;
		expressions.reserve(unpivot.entries.size());
		for (auto &entry : unpivot.entries) {
			expressions.push_back(make_unique<ColumnRefExpression>(entry.values[v_idx].ToString()));
		}
		unpivot_expressions.push_back(std::move(expressions));
	}

	// construct the UNNEST expression for the set of names (constant)
	auto unpivot_list = Value::LIST(LogicalType::VARCHAR, std::move(unpivot_names));
	auto unpivot_name_expr = make_unique<ConstantExpression>(std::move(unpivot_list));
	vector<unique_ptr<ParsedExpression>> unnest_name_children;
	unnest_name_children.push_back(std::move(unpivot_name_expr));
	auto unnest_name_expr = make_unique<FunctionExpression>("unnest", std::move(unnest_name_children));
	unnest_name_expr->alias = unpivot.unpivot_names[0];
	select_node->select_list.push_back(std::move(unnest_name_expr));

	// construct the UNNEST expression for the set of unpivoted columns
	if (ref.unpivot_names.size() != unpivot_expressions.size()) {
		throw BinderException("UNPIVOT name count mismatch - got %d names but %d expressions", ref.unpivot_names.size(),
		                      unpivot_expressions.size());
	}
	for (idx_t i = 0; i < unpivot_expressions.size(); i++) {
		auto list_expr = make_unique<FunctionExpression>("list_value", std::move(unpivot_expressions[i]));
		vector<unique_ptr<ParsedExpression>> unnest_val_children;
		unnest_val_children.push_back(std::move(list_expr));
		auto unnest_val_expr = make_unique<FunctionExpression>("unnest", std::move(unnest_val_children));
		auto unnest_name = i < ref.column_name_alias.size() ? ref.column_name_alias[i] : ref.unpivot_names[i];
		unnest_val_expr->alias = unnest_name;
		select_node->select_list.push_back(std::move(unnest_val_expr));
		if (!ref.include_nulls) {
			// if we are running with EXCLUDE NULLS we need to add an IS NOT NULL filter
			auto colref = make_unique<ColumnRefExpression>(unnest_name);
			auto filter = make_unique<OperatorExpression>(ExpressionType::OPERATOR_IS_NOT_NULL, std::move(colref));
			if (where_clause) {
				where_clause = make_unique<ConjunctionExpression>(ExpressionType::CONJUNCTION_AND,
				                                                  std::move(where_clause), std::move(filter));
			} else {
				where_clause = std::move(filter);
			}
		}
	}
	return select_node;
}

unique_ptr<BoundTableRef> Binder::Bind(PivotRef &ref) {
	if (!ref.source) {
		throw InternalException("Pivot without a source!?");
	}

	// bind the source of the pivot
	// we need to do this to be able to expand star expressions
	auto copied_source = ref.source->Copy();
	auto star_binder = Binder::CreateBinder(context);
	star_binder->Bind(*copied_source);

	// figure out the set of column names that are in the source of the pivot
	vector<unique_ptr<ParsedExpression>> all_columns;
	star_binder->ExpandStarExpression(make_unique<StarExpression>(), all_columns);

	unique_ptr<SelectNode> select_node;
	unique_ptr<ParsedExpression> where_clause;
	if (!ref.aggregates.empty()) {
		select_node = BindPivot(ref, std::move(all_columns));
	} else {
		select_node = BindUnpivot(*star_binder, ref, std::move(all_columns), where_clause);
	}
	// bind the generated select node
	auto child_binder = Binder::CreateBinder(context, this);
	auto bound_select_node = child_binder->BindNode(*select_node);
	auto root_index = bound_select_node->GetRootIndex();
	BoundQueryNode *bound_select_ptr = bound_select_node.get();

	unique_ptr<BoundTableRef> result;
	MoveCorrelatedExpressions(*child_binder);
	result = make_unique<BoundSubqueryRef>(std::move(child_binder), std::move(bound_select_node));
	auto alias = ref.alias.empty() ? "__unnamed_pivot" : ref.alias;
	SubqueryRef subquery_ref(nullptr, alias);
	subquery_ref.column_name_alias = std::move(ref.column_name_alias);
	if (where_clause) {
		// if a WHERE clause was provided - bind a subquery holding the WHERE clause
		// we need to bind a new subquery here because the WHERE clause has to be applied AFTER the unnest
		child_binder = Binder::CreateBinder(context, this);
		child_binder->bind_context.AddSubquery(root_index, subquery_ref.alias, subquery_ref, *bound_select_ptr);
		auto where_query = make_unique<SelectNode>();
		where_query->select_list.push_back(make_unique<StarExpression>());
		where_query->where_clause = std::move(where_clause);
		bound_select_node = child_binder->BindSelectNode(*where_query, std::move(result));
		bound_select_ptr = bound_select_node.get();
		root_index = bound_select_node->GetRootIndex();
		result = make_unique<BoundSubqueryRef>(std::move(child_binder), std::move(bound_select_node));
	}
	bind_context.AddSubquery(root_index, subquery_ref.alias, subquery_ref, *bound_select_ptr);
	return result;
}

} // namespace duckdb
