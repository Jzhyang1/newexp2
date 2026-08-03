// source: https://github.com/xrp-project/My-YCSB/blob/master/core/include/workload.h

#pragma once

#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <cmath>
#include <stdexcept>
#include <numeric>
#include <fstream>
#include <iostream>
#include <string>
#include <sstream>
#include <list>
#include <vector>
#include <mutex>
#include <memory>
#include <cstdint>
#include <unordered_map>

enum OperationType {
	UPDATE = 0,
	INSERT,
	READ,
	SCAN,
	READ_MODIFY_WRITE,
	NR_OP_TYPE,
};

extern const char* operation_type_name[];

struct Operation {
	OperationType type;
	uint64_t key;
	char *value_buffer;  /* for UPDATE, INSERT, and READ_MODIFY_WRITE */
	long value_buffer_size;
	char *reply_value_buffer;  /* for READ */
	long scan_length;  /* for SCAN */
	bool is_last_op;
};

struct OpProportion {
	float op[NR_OP_TYPE];
};

struct Workload {
	long value_size;
	bool record_keys = false;
	std::vector<unsigned long> recorded_keys;

	Workload(long value_size);
	virtual ~Workload() = default;
	virtual void next_op(Operation *op) = 0;
	virtual bool has_next_op() = 0;

protected:
	static long generate_random_long(unsigned int *seedp);
	static double generate_random_double(unsigned int *seedp);
};

struct UniformWorkload : public Workload {
	/* configuration */
	long nr_entry;
	long nr_op;
	long scan_length;
	struct OpProportion op_prop;

	/* states */
	unsigned int seed;
	long cur_nr_op;

	UniformWorkload(long value_size, long scan_length, long nr_entry, long nr_op, struct OpProportion op_prop, unsigned int seed);
	void next_op(Operation *op) override;
	bool has_next_op() override;

private:
	void generate_value_string(char *value_buffer);
};

struct ZipfianWorkload : public Workload {
	/* configuration */
	long nr_entry;
	long nr_op;
	long scan_length;
	struct OpProportion op_prop;
	double zipfian_constant;

	/* states */
	unsigned int seed;
	long cur_nr_op;

	double zetan;
	double theta;
	double zeta2theta;
	double alpha;
	double eta;

	ZipfianWorkload(long value_size, long scan_length, long nr_entry, long nr_op, struct OpProportion op_prop, double zipfian_constant, unsigned int seed);
	void next_op(Operation *op) override;
	bool has_next_op() override;
	ZipfianWorkload *clone(unsigned int new_seed);

private:
	static unsigned long fnv1_64_hash(unsigned long value);
	unsigned long generate_zipfian_random_ulong(bool hash);
	void generate_value_string(char *value_buffer);
};

struct ScanWorkload : public Workload {
	/* configuration */
	long nr_entry;
	long start_key;

	/* constants */

	/* states */
	unsigned int seed;
	long cur_nr_entry;
	std::mutex lock;


	ScanWorkload(long nr_entry, long start_key, long value_size, unsigned int seed);
	void next_op(Operation *op) override;
	bool has_next_op() override;
private:
	bool has_next_op_unsafe();
	void generate_value_string(char *value_buffer);
};

struct ReaderTraceWorkload : public Workload {
	/* states */
	std::ifstream source;
	long cur_nr_entry;
	std::mutex lock;


	ReaderTraceWorkload(std::string path, long value_size);
	void next_op(Operation *op) override;
	bool has_next_op() override;

private:
    std::uint64_t _next_op;	// pre-read
	bool _has_next_op;
	bool has_next_op_unsafe();
    bool next_op_unsafe();	// returns true if we are at the last op
};

struct LatestWorkload : public Workload {
	/* configuration */
	long nr_entry;
	long nr_op;
	double read_ratio;
	double zipfian_constant;

	/* states */
	unsigned int seed;
	long cur_nr_op;
	unsigned long cur_ack_key;

	double zetan;
	double theta;
	double zeta2theta;
	double alpha;
	double eta;

	LatestWorkload(long value_size, long nr_entry, long nr_op, double read_ratio, double zipfian_constant, unsigned int seed);
	void next_op(Operation *op) override;
	bool has_next_op() override;
	LatestWorkload *clone(unsigned int new_seed);

private:
	static unsigned long fnv1_64_hash(unsigned long value);
	unsigned long generate_zipfian_random_ulong(bool hash);
	void generate_value_string(char *value_buffer);
};

// Non-parametric, read-only workload: fits a lightweight Markov model from
// the first `train_samples` pages of another workload's own output, then
// generates new page requests from that fitted model instead of replaying
// the source. Gives prefetchers that mine page-follows-page associations
// (see policies/assoc_miner.h) genuine sequential structure to learn, unlike
// the i.i.d. Zipfian/Latest draws the source workloads normally produce.
//
// Three count tables are fit from consecutive pages (page[i-1] -> page[i])
// and their deltas (delta[i] = page[i] - page[i-1]):
//   - direct[P]      -> {Q: count}   how often Q follows P directly
//   - rel_to_page[D]  -> {Q: count}   how often Q follows a jump of size D
//   - rel_to_rel[D]   -> {D': count}  how often jump D' follows jump D
// Generation draws two independent Bernoulli(relative_weight) coins: the
// first picks "relative" vs "direct" (table 1); the second, only when
// "relative", picks rel_to_rel vs rel_to_page. That makes
// P(rel_to_rel) = relative_weight^2, P(rel_to_page) = relative_weight *
// (1 - relative_weight), P(direct) = 1 - relative_weight.
struct MarkovWorkload : public Workload {
	/* configuration */
	long nr_op;
	uint64_t page_span;
	double relative_weight;

	/* states */
	unsigned int seed;
	long cur_nr_op;
	uint64_t cur_page;
	int64_t cur_delta;

	// Takes ownership of `source`: pulls exactly `train_samples` ops from it
	// to fit the model, then deletes it -- `source` is never touched again.
	MarkovWorkload(long value_size, Workload* source, long train_samples, long nr_op,
	               uint64_t page_span, double relative_weight, unsigned int seed);
	void next_op(Operation *op) override;
	bool has_next_op() override;

private:
	std::unordered_map<uint64_t, std::unordered_map<uint64_t, uint64_t>> direct_;
	std::unordered_map<int64_t, std::unordered_map<uint64_t, uint64_t>> rel_to_page_;
	std::unordered_map<int64_t, std::unordered_map<int64_t, uint64_t>> rel_to_rel_;

	bool try_direct(uint64_t &next_page);
	bool try_rel_to_page(uint64_t &next_page);
	bool try_rel_to_rel(uint64_t &next_page);
	uint64_t wrap_page(int64_t page) const;
};