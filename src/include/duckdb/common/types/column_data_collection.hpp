//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/types/column_data_collection.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "duckdb/common/unordered_set.hpp"
#include "duckdb/common/mutex.hpp"
#include <functional>

namespace duckdb {
class BufferManager;
class BlockHandle;
class ClientContext;
struct ColumnDataCopyFunction;
class ColumnDataAllocator;
class ColumnDataCollectionSegment;

enum class ColumnDataAllocatorType { BUFFER_MANAGER_ALLOCATOR, IN_MEMORY_ALLOCATOR };

struct ChunkManagementState {
	unordered_map<idx_t, BufferHandle> handles;
};

struct ColumnDataAppendState {
	ChunkManagementState current_chunk_state;
	vector<VectorData> vector_data;
};

struct ColumnDataScanState {
	ChunkManagementState current_chunk_state;
	idx_t segment_index;
	idx_t chunk_index;
	idx_t current_row_index;
	idx_t next_row_index;
};

struct ColumnDataParallelScanState {
	ColumnDataScanState scan_state;
	mutex lock;
};

struct ColumnDataLocalScanState {
	ChunkManagementState current_chunk_state;
	idx_t current_row_index;
};

//! The ColumnDataCollection represents a set of (buffer-managed) data stored in columnar format
//! It is efficient to read and scan
class ColumnDataCollection {
public:
	//! Constructs an in-memory column data collection from an allocator
	ColumnDataCollection(Allocator &allocator, vector<LogicalType> types);
	//! Constructs a buffer-managed column data collection
	ColumnDataCollection(BufferManager &buffer_manager, vector<LogicalType> types);
	//! Constructs either an in-memory or a buffer-managed column data collection
	ColumnDataCollection(ClientContext &context, vector<LogicalType> types,
	                     ColumnDataAllocatorType type = ColumnDataAllocatorType::BUFFER_MANAGER_ALLOCATOR);
	//! Creates a column data collection that inherits the blocks to write to. This allows blocks to be shared
	//! between multiple column data collections and prevents wasting space.
	//! Note that after one CDC inherits blocks from another, the other
	//! cannot be written to anymore (i.e. we take ownership of the half-written blocks).
	ColumnDataCollection(ColumnDataCollection &parent);
	ColumnDataCollection(shared_ptr<ColumnDataAllocator> allocator, vector<LogicalType> types);
	~ColumnDataCollection();

public:
	DUCKDB_API vector<LogicalType> &Types() {
		return types;
	}
	const vector<LogicalType> &Types() const {
		return types;
	}

	//! The amount of rows in the ColumnDataCollection
	DUCKDB_API const idx_t &Count() const {
		return count;
	}

	//! The amount of columns in the ColumnDataCollection
	DUCKDB_API idx_t ColumnCount() const {
		return types.size();
	}

	//! Initializes an Append state - useful for optimizing many appends made to the same column data collection
	DUCKDB_API void InitializeAppend(ColumnDataAppendState &state);
	//! Append a DataChunk to this ColumnDataCollection using the specified append state
	DUCKDB_API void Append(ColumnDataAppendState &state, DataChunk &new_chunk);

	//! Initializes a chunk with the correct types that can be used to call Scan
	DUCKDB_API void InitializeScanChunk(DataChunk &chunk) const;
	//! Initializes a Scan state
	DUCKDB_API void InitializeScan(ColumnDataScanState &state) const;
	//! Initialize a parallel scan over the column data collection
	DUCKDB_API void InitializeScan(ColumnDataParallelScanState &state) const;
	//! Scans a DataChunk from the ColumnDataCollection
	DUCKDB_API bool Scan(ColumnDataScanState &state, DataChunk &result) const;
	//! Scans a DataChunk from the ColumnDataCollection
	DUCKDB_API bool Scan(ColumnDataParallelScanState &state, ColumnDataLocalScanState &lstate, DataChunk &result) const;

	//! Performs a scan of the ColumnDataCollection, invoking the callback for each chunk
	DUCKDB_API void Scan(const std::function<void(DataChunk &)> &callback);

	//! Append a DataChunk directly to this ColumnDataCollection - calls InitializeAppend and Append internally
	DUCKDB_API void Append(DataChunk &new_chunk);

	//! Appends the other ColumnDataCollection to this, destroying the other data collection
	DUCKDB_API void Combine(ColumnDataCollection &other);

	DUCKDB_API void Verify();

	DUCKDB_API string ToString() const;
	DUCKDB_API void Print() const;

	DUCKDB_API idx_t ChunkCount() const;

	DUCKDB_API void Reset();

private:
	//! Initialize the column data collection
	void Initialize(vector<LogicalType> types);

	//! Creates a new segment within the ColumnDataCollection
	void CreateSegment();

	static ColumnDataCopyFunction GetCopyFunction(const LogicalType &type);

	//! Obtains the next scan index to scan from
	bool NextScanIndex(ColumnDataScanState &state, idx_t &chunk_index, idx_t &segment_index, idx_t &row_index) const;

private:
	//! The Column Data Allocator
	buffer_ptr<ColumnDataAllocator> allocator;
	//! The types of the stored entries
	vector<LogicalType> types;
	//! The number of entries stored in the column data collection
	idx_t count;
	//! The data segments of the column data collection
	vector<unique_ptr<ColumnDataCollectionSegment>> segments;
	//! The set of copy functions
	vector<ColumnDataCopyFunction> copy_functions;
	//! When the column data collection is marked as finished - new tuples can no longer be appended to it
	bool finished_append;
};

} // namespace duckdb
