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

// Non-parametric, read-only workload: fits a lightweight model from the first
// `train_samples` pages of another workload's own output, then generates new
// page requests via a Metropolis-Hastings (MH) random walk instead of
// replaying the source. Gives prefetchers that mine page-follows-page
// associations (see policies/assoc_miner.h) genuine *local* sequential
// structure to learn, while -- unlike naively composing observed jumps --
// provably converging to the source's own page-popularity distribution
// (empirically, visits_[page]/train_samples) as its stationary distribution.
// That matters because the source's popularity concentration (e.g.
// Zipfian's hot set) is what actually drives cache hit ratio; a generator
// that wanders off it produces a much harder, unrealistic workload. See
// README.md's "Markov workload" section for the derivation.
//
// Two tables are fit from the training pages:
//   - direct[P]     -> {Q: count}  how often Q follows P directly
//   - delta_freq[D] -> count       how often a jump of (signed) size D was
//                                  observed between consecutive pages,
//                                  *symmetrized*: every observed d also adds
//                                  a count to -d, so delta_freq[D] ==
//                                  delta_freq[-D] always, by construction.
// plus a marginal visits_[P] -> count (the empirical target distribution
// pi(P), used only to accept/reject -- never to propose).
//
// Each generation step draws Bernoulli(relative_weight) to pick a proposal
// kernel -- "relative" (propose cur_page + D via delta_freq) or "direct"
// (propose Q via direct[cur_page]) -- then accepts with the MH ratio
// min(1, pi(candidate)*q(cur|candidate) / (pi(cur)*q(candidate|cur))); on
// rejection the walk self-loops (repeats cur_page). For "direct", q(cur|
// candidate) is the reverse lookup direct[candidate][cur_page]/total. For
// "relative", delta_freq's symmetry makes q(cur|candidate) == q(candidate|
// cur) identically, so no lookup is needed -- this is deliberate, and not
// just a simplification: an earlier version conditioned delta_freq on the
// *incoming* jump size (a proper 2nd-order relative-step model) and
// evaluated its reverse via a per-incoming-delta table, but jump sizes
// between hash-scrambled pages (e.g. Zipfian's key hashing) are almost all
// numerically unique, so the exact negated delta the reverse lookup needed
// almost never existed -- acceptance collapsed to ~0 and the walk got stuck
// self-looping (measured: 3 distinct pages touched in 20k generated ops).
// Pooling and symmetrizing trades that 2nd-order conditioning for actually
// working: candidates stay reachable and the walk's long-run page-visit
// frequency converges to pi as intended. See README.md for the full
// derivation and that failure mode.
//
// A third kernel from an earlier version -- predicting an absolute page
// purely from the previous jump size, ignoring current position -- is
// dropped entirely: it has no well-defined reverse move to Hastings-correct
// against, so it can't be made MH-valid without discarding exactly the
// property (page-identity-agnostic jumps) that defined it.
//
// If neither kernel has an entry for the current state (e.g. right after
// seeding), the step falls back to an independence proposal drawn directly
// from visits_ -- which, proposing exactly from pi, is always accepted.
struct MarkovWorkload : public Workload {
	/* configuration */
	long nr_op;
	uint64_t page_span;
	double relative_weight;

	/* states */
	unsigned int seed;
	long cur_nr_op;
	uint64_t cur_page;

	// Takes ownership of `source`: pulls exactly `train_samples` ops from it
	// to fit the model, then deletes it -- `source` is never touched again.
	MarkovWorkload(long value_size, Workload* source, long train_samples, long nr_op,
	               uint64_t page_span, double relative_weight, unsigned int seed);
	void next_op(Operation *op) override;
	bool has_next_op() override;

private:
	std::unordered_map<uint64_t, std::unordered_map<uint64_t, uint64_t>> direct_;
	std::unordered_map<int64_t, uint64_t> delta_freq_;
	std::unordered_map<uint64_t, uint64_t> visits_;

	// Proposes a candidate (and its forward/reverse proposal densities) from
	// the given kernel; returns false if the kernel has no entry to propose
	// from (only possible for "direct", pre-seed/pre-any-training-pair).
	bool propose_direct(uint64_t &candidate, double &q_fwd, double &q_rev);
	bool propose_relative(uint64_t &candidate, double &q_fwd, double &q_rev);
	uint64_t sample_from_visits();
	uint64_t wrap_page(int64_t page) const;
};