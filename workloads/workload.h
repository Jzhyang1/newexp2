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