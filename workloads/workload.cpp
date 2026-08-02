#include <vector>
#include <random>
#include <algorithm>
#include "workload.h"

const char* operation_type_name[] = {
	"UPDATE", "INSERT", "READ", "SCAN", "READ_MODIFY_WRITE"
};

Workload::Workload(long value_size)
: value_size(value_size) {
	;
}

long Workload::generate_random_long(unsigned int *seedp) {
	return (((long)rand_r(seedp)) << (sizeof(int) * 8)) | rand_r(seedp);
}

double Workload::generate_random_double(unsigned int *seedp) {
	return ((double)rand_r(seedp)) / RAND_MAX;
}

UniformWorkload::UniformWorkload(long value_size, long scan_length, long nr_entry,
                                 long nr_op, struct OpProportion op_prop, unsigned int seed)
: Workload(value_size), scan_length(scan_length), nr_entry(nr_entry), nr_op(nr_op), op_prop(op_prop), seed(seed), cur_nr_op(0) {
}

bool UniformWorkload::has_next_op() {
	return this->cur_nr_op < this->nr_op;
}

void UniformWorkload::next_op(Operation *op) {
	if (!this->has_next_op())
		throw std::invalid_argument("does not have next op");
	double op_random = this->generate_random_double(&this->seed);
	int op_random_int = 1 + (int) (op_random * 100);
	int running_sum = 0;
	if (running_sum += int(this->op_prop.op[UPDATE] * 100), op_random_int <= running_sum) {
		op->type = UPDATE;
		this->generate_value_string(op->value_buffer);
	} else if (running_sum += int(this->op_prop.op[INSERT] * 100), op_random_int <= running_sum) {
		op->type = INSERT;
		this->generate_value_string(op->value_buffer);
	} else if (running_sum += int(this->op_prop.op[READ] * 100), op_random_int <= running_sum) {
		op->type = READ;
	} else if (running_sum += int(this->op_prop.op[SCAN] * 100), op_random_int <= running_sum) {
		op->type = SCAN;
		op->scan_length = this->scan_length;
	} else if (running_sum += int(this->op_prop.op[READ_MODIFY_WRITE] * 100), op_random_int <= running_sum) {
		op->type = READ_MODIFY_WRITE;
		this->generate_value_string(op->value_buffer);
	} else {
		printf("op_random_int = %d, running_sum = %d, op_random_int == running_sum: %d\n", op_random_int, running_sum, op_random_int == running_sum);
		printf("op_prop = %f, %f, %f, %f, %f\n", this->op_prop.op[UPDATE], this->op_prop.op[INSERT], this->op_prop.op[READ], this->op_prop.op[SCAN], this->op_prop.op[READ_MODIFY_WRITE]);
		throw std::invalid_argument("failed to generate an operation");
	}
	long key = this->generate_random_long(&this->seed) % this->nr_entry;
	op->key = key;
	++this->cur_nr_op;
	op->is_last_op = !this->has_next_op();
}

void UniformWorkload::generate_value_string(char *value_buffer) {
	for (int i = 0; i < this->value_size - 1; ++i) {
		value_buffer[i] = 'a' + (rand_r(&this->seed) % ('z' - 'a' + 1));
	}
	value_buffer[this->value_size - 1] = '\0';
}

ZipfianWorkload::ZipfianWorkload(long value_size, long scan_length, long nr_entry, long nr_op,
                                 struct OpProportion op_prop, double zipfian_constant, unsigned int seed)
: Workload(value_size), scan_length(scan_length), nr_entry(nr_entry), nr_op(nr_op), op_prop(op_prop),
  zipfian_constant(zipfian_constant), seed(seed), cur_nr_op(0) {
	/* zipfian-related initialization */
	this->zetan = 0;
	for (long i = 1; i < this->nr_entry + 1; ++i) {
		this->zetan += 1.0 / (pow((double) i, this->zipfian_constant));
	}
	this->theta = this->zipfian_constant;
	this->zeta2theta = 0;
	for (long i = 1; i < 3; ++i) {
		this->zeta2theta += 1.0 / (pow((double) i, this->zipfian_constant));
	}
	this->alpha = 1.0 / (1.0 - this->theta);
	this->eta = (1 - pow(2.0 / (double) this->nr_entry, 1 - this->theta))
	            / (1 - (this->zeta2theta / this->zetan));
	this->generate_zipfian_random_ulong(true);
}

bool ZipfianWorkload::has_next_op() {
	return this->cur_nr_op < this->nr_op;
}


void ZipfianWorkload::next_op(Operation *op) {
	if (!this->has_next_op())
		throw std::invalid_argument("does not have next op");
	double op_random = this->generate_random_double(&this->seed);
	int op_random_int = 1 + (int) (op_random * 100);
	int running_sum = 0;
	if (running_sum += int(this->op_prop.op[UPDATE] * 100), /*this->op_prop.op[UPDATE] != 0 && */ op_random_int <= running_sum) {
		op->type = UPDATE;
		this->generate_value_string(op->value_buffer);
	} else if (running_sum += int(this->op_prop.op[INSERT] * 100), /*this->op_prop.op[INSERT] != 0 &&*/ op_random_int <= running_sum) {
		op->type = INSERT;
		this->generate_value_string(op->value_buffer);
	} else if (running_sum += int(this->op_prop.op[READ] * 100), /*this->op_prop.op[READ] != 0 &&*/ op_random_int <= running_sum) {
		op->type = READ;
	} else if (running_sum += int(this->op_prop.op[SCAN] * 100), /*this->op_prop.op[SCAN] != 0 &&*/ op_random_int <= running_sum) {
		op->type = SCAN;
		op->scan_length = this->scan_length;
	} else if (running_sum += int(this->op_prop.op[READ_MODIFY_WRITE] * 100), /*this->op_prop.op[READ_MODIFY_WRITE] != 0 &&*/ op_random_int <= running_sum) {
		op->type = READ_MODIFY_WRITE;
		this->generate_value_string(op->value_buffer);
	} else {
		printf("op_random_int = %d, running_sum = %d, op_random_int == running_sum: %d\n", op_random_int, running_sum, op_random_int == running_sum);
		printf("op_prop = %f, %f, %f, %f, %f\n", this->op_prop.op[UPDATE], this->op_prop.op[INSERT], this->op_prop.op[READ], this->op_prop.op[SCAN], this->op_prop.op[READ_MODIFY_WRITE]);
		throw std::invalid_argument("failed to generate an operation");
	}
	unsigned long key = this->generate_zipfian_random_ulong(true) % ((unsigned long) this->nr_entry);
	// If record_keys is set to true, record op's key
	if (this->record_keys) {
		this->recorded_keys.push_back(key);
	}
	op->key = key;
	++this->cur_nr_op;
	op->is_last_op = !this->has_next_op();
}

ZipfianWorkload * ZipfianWorkload::clone(unsigned int new_seed) {
	/* create a new ZipfianWorkload with a cheap nr_entry */
	ZipfianWorkload *copy = new ZipfianWorkload(this->value_size, this->scan_length, 3, this->nr_op,
	                                            this->op_prop, this->zipfian_constant, new_seed);
	copy->zetan = this->zetan;
	copy->theta = this->theta;
	copy->zeta2theta = this->zeta2theta;
	copy->alpha = this->alpha;
	copy->eta = this->eta;
	copy->nr_entry = this->nr_entry;
	copy->record_keys = this->record_keys;
	return copy;
}

unsigned long ZipfianWorkload::fnv1_64_hash(unsigned long value) {
	uint64_t hash = 14695981039346656037ul;
	uint8_t *p = (uint8_t *) &value;
	for (int i = 0; i < sizeof(unsigned long); ++i, ++p) {
		hash *= 1099511628211ul;
		hash ^= *p;
	}
	return (unsigned long) hash;
}

unsigned long ZipfianWorkload::generate_zipfian_random_ulong(bool hash) {
	double u = this->generate_random_double(&this->seed);
	double uz = u * this->zetan;
	if (uz < 1)
		return 0;
	if (uz < 1 + pow(0.5, this->theta))
		return 1;
	unsigned long ret = (unsigned long) ((double)this->nr_entry * pow(this->eta * u - this->eta + 1, this->alpha));
	if (hash)
		return ZipfianWorkload::fnv1_64_hash(ret);
	else
		return ret;
}

void ZipfianWorkload::generate_value_string(char *value_buffer) {
	for (int i = 0; i < this->value_size - 1; ++i) {
		value_buffer[i] = 'a' + (rand_r(&this->seed) % ('z' - 'a' + 1));
	}
	value_buffer[this->value_size - 1] = '\0';
}

ScanWorkload::ScanWorkload(long nr_entry, long start_key, long value_size, unsigned int seed)
: Workload(value_size), nr_entry(nr_entry), start_key(start_key), cur_nr_entry(0), seed(seed) {
	;
}

bool ScanWorkload::has_next_op() {
	this->lock.lock();
	auto ret = this->cur_nr_entry < this->nr_entry;
	this->lock.unlock();
	return ret;
}

bool ScanWorkload::has_next_op_unsafe() {
	return this->cur_nr_entry < this->nr_entry;
}

void ScanWorkload::next_op(Operation *op) {
	this->lock.lock();
	if (!this->has_next_op_unsafe())
		throw std::invalid_argument("does not have next op");
	op->type = SCAN;
	op->key = this->start_key + this->cur_nr_entry++;
	op->is_last_op = !this->has_next_op_unsafe();
	this->lock.unlock();
}

void ScanWorkload::generate_value_string(char *value_buffer) {
	for (int i = 0; i < this->value_size - 1; ++i) {
		value_buffer[i] = 'a' + (rand_r(&this->seed) % ('z' - 'a' + 1));
	}
	value_buffer[this->value_size - 1] = '\0';
}


ReaderTraceWorkload::ReaderTraceWorkload(std::string path, long value_size)
: Workload(value_size), source(path), _has_next_op(true) {
	if (!this->source.is_open()) {
		throw std::runtime_error("failed to open trace file: " + path);
	}
	this->next_op_unsafe();
}

bool ReaderTraceWorkload::has_next_op() {
	this->lock.lock();
	auto ret = this->has_next_op_unsafe();
	this->lock.unlock();
	return ret;
}

bool ReaderTraceWorkload::next_op_unsafe() {
	if (!this->source.eof() && (this->source >> this->_next_op)) {
		this->_has_next_op = true;
	} else {
		this->_next_op = -1;
		this->_has_next_op = false;
	}
	return this->_has_next_op;
}

bool ReaderTraceWorkload::has_next_op_unsafe() {
	return this->_has_next_op;
}

void ReaderTraceWorkload::next_op(Operation *op) {
	this->lock.lock();
	op->type = SCAN;
	op->key = this->_next_op;
	op->is_last_op = !this->next_op_unsafe();
	this->lock.unlock();
}

LatestWorkload::LatestWorkload(long value_size, long nr_entry, long nr_op, double read_ratio,
                               double zipfian_constant, unsigned int seed)
	: Workload(value_size), nr_entry(nr_entry), nr_op(nr_op), read_ratio(read_ratio),
	  zipfian_constant(zipfian_constant), seed(seed), cur_nr_op(0), cur_ack_key(0) {

	/* zipfian-related initialization */
	this->zetan = 0;
	for (long i = 1; i < this->nr_entry + 1; ++i) {
		this->zetan += 1.0 / (pow((double) i, this->zipfian_constant));
	}
	this->theta = this->zipfian_constant;
	this->zeta2theta = 0;
	for (long i = 1; i < 3; ++i) {
		this->zeta2theta += 1.0 / (pow((double) i, this->zipfian_constant));
	}
	this->alpha = 1.0 / (1.0 - this->theta);
	this->eta = (1 - pow(2.0 / (double) this->nr_entry, 1 - this->theta))
		    / (1 - (this->zeta2theta / this->zetan));
	this->generate_zipfian_random_ulong(true);
}

bool LatestWorkload::has_next_op() {
	return this->cur_nr_op < this->nr_op;
}

void LatestWorkload::next_op(Operation *op) {
	if (!this->has_next_op())
		throw std::invalid_argument("does not have next op");
	bool read = this->generate_random_double(&this->seed) <= this->read_ratio;
	if (this->cur_ack_key == 0) {
		read = false;
	}
	unsigned long key;
	if (read) {
		op->type = READ;
		key = this->generate_zipfian_random_ulong(false) % (this->cur_ack_key);
		key = this->cur_ack_key - key - 1;
	} else {
		if (this->cur_ack_key >= this->nr_entry) {
			op->type = UPDATE;
			key = this->generate_zipfian_random_ulong(false) % (this->cur_ack_key);
			key = this->cur_ack_key - key - 1;
		} else {
			op->type = INSERT;
			key = this->cur_ack_key++;
		}
	}
	key = LatestWorkload::fnv1_64_hash(key) % ((unsigned long) this->nr_entry);
	op->key = key;
	if (!read)
		this->generate_value_string(op->value_buffer);
	++this->cur_nr_op;
	op->is_last_op = !this->has_next_op();
}

LatestWorkload * LatestWorkload::clone(unsigned int new_seed) {
	/* create a new ZipfianWorkload with a cheap nr_entry */
	LatestWorkload *copy = new LatestWorkload(this->value_size, 3, this->nr_op,
						  this->read_ratio, this->zipfian_constant, new_seed);
	copy->zetan = this->zetan;
	copy->theta = this->theta;
	copy->zeta2theta = this->zeta2theta;
	copy->alpha = this->alpha;
	copy->eta = this->eta;
	copy->nr_entry = this->nr_entry;
	return copy;
}

unsigned long LatestWorkload::fnv1_64_hash(unsigned long value) {
	uint64_t hash = 14695981039346656037ul;
	uint8_t *p = (uint8_t *) &value;
	for (int i = 0; i < sizeof(unsigned long); ++i, ++p) {
		hash *= 1099511628211ul;
		hash ^= *p;
	}
	return (unsigned long) hash;
}

unsigned long LatestWorkload::generate_zipfian_random_ulong(bool hash) {
	double u = this->generate_random_double(&this->seed);
	double uz = u * this->zetan;
	if (uz < 1)
		return 0;
	if (uz < 1 + pow(0.5, this->theta))
		return 1;
	unsigned long ret = (unsigned long) ((double)this->nr_entry * pow(this->eta * u - this->eta + 1, this->alpha));
	if (hash)
		return LatestWorkload::fnv1_64_hash(ret);
	else
		return ret;
}

void LatestWorkload::generate_value_string(char *value_buffer) {
	for (int i = 0; i < this->value_size - 1; ++i) {
		value_buffer[i] = 'a' + (rand_r(&this->seed) % ('z' - 'a' + 1));
	}
	value_buffer[this->value_size - 1] = '\0';
}