import time
import numpy as np
import faiss

# 1. Configuration (Simulating a modern RAG or search dataset)
dimension = 1536      # Standard dimension for modern embedding models (like OpenAI text-embedding-3-large)
database_size = 50000 # Number of vectors to index
query_size = 100      # Number of parallel search queries

print(f"Generating {database_size} mock AI vectors with {dimension} dimensions...")
# Generate random floating-point data (simulating embeddings)
np.random.seed(42)
data = np.random.random((database_size, dimension)).astype('float32')
queries = np.random.random((query_size, dimension)).astype('float32')

# 2. Heavy CPU Workload: Building the HNSW Index
# HNSW is a graph-based structure that is highly CPU-intensive and utilizes multi-threading.
print("\n[CPU Workload 1] Building HNSW graph index...")
start_time = time.time()

# IndexHNSWFlat creates a graph structure where nodes are vectors
index = faiss.IndexHNSWFlat(dimension, 32) # 32 links per node
index.hnsw.efConstruction = 64            # Higher number = deeper graph exploration
index.add(data)                           # This utilizes 100% of available CPU cores

build_time = time.time() - start_time
print(f"Index built successfully in {build_time:.2f} seconds.")

# 3. Heavy CPU Workload: Multi-threaded Vector Search
print(f"\n[CPU Workload 2] Querying nearest neighbors for {query_size} vectors...")
start_time = time.time()

# Search the top 5 closest vectors for all queries simultaneously
k = 5
distances, indices = index.search(queries, k)

search_time = time.time() - start_time
print(f"Search completed in {search_time:.4f} seconds.")
print(f"Average search latency per query: {(search_time / query_size) * 1000:.2f} milliseconds.")
