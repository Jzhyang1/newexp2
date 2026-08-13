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

namespace {

// Draws from `candidates` proportional to count, using the pre-drawn uniform
// `r` in [0,1]. Returns false if `candidates` is empty. Duplicated for
// uint64_t/int64_t keys rather than templated, matching this file's style.
bool pick_weighted(const std::unordered_map<uint64_t, uint64_t> &candidates, double r, uint64_t *out) {
	if (candidates.empty()) return false;
	uint64_t total = 0;
	for (const auto &kv : candidates) total += kv.second;
	uint64_t target = (uint64_t)(r * (double)total);
	if (target >= total) target = total - 1;
	uint64_t cum = 0;
	for (const auto &kv : candidates) {
		cum += kv.second;
		if (target < cum) {
			*out = kv.first;
			return true;
		}
	}
	*out = candidates.begin()->first;  // unreachable
	return true;
}

bool pick_weighted(const std::unordered_map<int64_t, uint64_t> &candidates, double r, int64_t *out) {
	if (candidates.empty()) return false;
	uint64_t total = 0;
	for (const auto &kv : candidates) total += kv.second;
	uint64_t target = (uint64_t)(r * (double)total);
	if (target >= total) target = total - 1;
	uint64_t cum = 0;
	for (const auto &kv : candidates) {
		cum += kv.second;
		if (target < cum) {
			*out = kv.first;
			return true;
		}
	}
	*out = candidates.begin()->first;  // unreachable
	return true;
}

// Total count across a table -- the normalizer for both sampling and for
// evaluating a specific entry's proposal probability.
uint64_t total_count(const std::unordered_map<uint64_t, uint64_t> &table) {
	uint64_t total = 0;
	for (const auto &kv : table) total += kv.second;
	return total;
}

uint64_t total_count(const std::unordered_map<int64_t, uint64_t> &table) {
	uint64_t total = 0;
	for (const auto &kv : table) total += kv.second;
	return total;
}

}  // namespace

MarkovWorkload::MarkovWorkload(long value_size, Workload *source, long train_samples, long nr_op,
                                uint64_t page_span, double relative_weight, unsigned int seed)
: Workload(value_size), nr_op(nr_op), page_span(page_span), relative_weight(relative_weight),
  seed(seed), cur_nr_op(0) {
	if (train_samples < 3) {
		delete source;
		throw std::invalid_argument("MarkovWorkload requires at least 3 training samples");
	}

	// `source` may write into op.value_buffer for non-read op types (e.g.
	// LatestWorkload's INSERT/UPDATE branch); size the scratch buffer to
	// match, same reasoning as app.cpp's kScratchValueSize.
	std::vector<char> value_scratch(source->value_size > 0 ? (std::size_t)source->value_size : 1);
	Operation scratch{};
	scratch.value_buffer = value_scratch.data();

	std::vector<uint64_t> pages;
	pages.reserve((std::size_t)train_samples);
	for (long i = 0; i < train_samples; ++i) {
		if (!source->has_next_op()) {
			delete source;
			throw std::runtime_error("MarkovWorkload: source workload ran out of ops before "
			                          "producing the requested training samples");
		}
		source->next_op(&scratch);
		pages.push_back(scratch.key);
	}
	delete source;

	for (std::size_t i = 0; i < pages.size(); ++i) {
		this->visits_[pages[i]]++;
		if (i >= 1) {
			uint64_t a = pages[i - 1], b = pages[i];
			// Symmetrize the edge itself (direct_[a][b] == direct_[b][a]
			// always), not just hope the reverse pair also got observed --
			// see the class comment on why an empirical-only reverse lookup
			// is too sparse to be usable at realistic training sizes.
			this->direct_[a][b]++;
			if (a != b) this->direct_[b][a]++;
			int64_t d = (int64_t)b - (int64_t)a;
			this->delta_freq_[d]++;
			this->delta_freq_[-d]++;  // symmetrize: makes delta_freq_[D] == delta_freq_[-D]
		}
	}

	this->cur_page = pages.back();
}

bool MarkovWorkload::has_next_op() {
	return this->cur_nr_op < this->nr_op;
}

uint64_t MarkovWorkload::wrap_page(int64_t page) const {
	int64_t span = (int64_t)this->page_span;
	int64_t wrapped = page % span;
	if (wrapped < 0) wrapped += span;
	return (uint64_t)wrapped;
}

uint64_t MarkovWorkload::sample_from_visits() {
	double r = this->generate_random_double(&this->seed);
	uint64_t out;
	pick_weighted(this->visits_, r, &out);  // visits_ is never empty past construction
	return out;
}

// direct[cur_page] -> {Q: count} proposes Q with q_fwd = count/total(direct[
// cur_page]). direct_ is symmetrized at construction time (direct_[A][B] ==
// direct_[B][A] always -- see the constructor), so the reverse edge weight
// is exactly the same count `w` used for q_fwd; only the *far* side's total
// (Q's own out-degree, generally different from cur_page's) can differ. This
// is what makes q_rev always well-defined instead of depending on the data
// happening to independently confirm the reverse pair, which at realistic
// training sizes it almost never does (most edges are singleton
// observations) -- exactly the failure mode that made an earlier,
// non-symmetrized version of this table collapse to near-zero acceptance.
bool MarkovWorkload::propose_direct(uint64_t &candidate, double &q_fwd, double &q_rev) {
	auto it = this->direct_.find(this->cur_page);
	if (it == this->direct_.end() || it->second.empty()) return false;

	double r = this->generate_random_double(&this->seed);
	uint64_t q;
	pick_weighted(it->second, r, &q);
	uint64_t w = it->second.at(q);
	q_fwd = (double)w / (double)total_count(it->second);

	candidate = q;

	auto rev_it = this->direct_.find(q);
	uint64_t total_cand = (rev_it == this->direct_.end()) ? 0 : total_count(rev_it->second);
	q_rev = (total_cand > 0) ? (double)w / (double)total_cand : 0.0;
	return true;
}

// delta_freq -> {D: count} proposes candidate = cur_page + D with q_fwd =
// count(D)/total. Since delta_freq is symmetrized at construction time
// (delta_freq[D] == delta_freq[-D] always), q_rev == q_fwd identically --
// no reverse lookup needed, and none of the "exact negation must also have
// been observed" fragility the earlier per-incoming-delta version had.
bool MarkovWorkload::propose_relative(uint64_t &candidate, double &q_fwd, double &q_rev) {
	if (this->delta_freq_.empty()) return false;

	double r = this->generate_random_double(&this->seed);
	int64_t delta;
	pick_weighted(this->delta_freq_, r, &delta);
	q_fwd = (double)this->delta_freq_.at(delta) / (double)total_count(this->delta_freq_);
	q_rev = q_fwd;

	candidate = this->wrap_page((int64_t)this->cur_page + delta);
	return true;
}

void MarkovWorkload::next_op(Operation *op) {
	if (!this->has_next_op())
		throw std::invalid_argument("does not have next op");

	double coin = this->generate_random_double(&this->seed);
	bool use_relative = coin < this->relative_weight;

	uint64_t candidate;
	double q_fwd = 0.0, q_rev = 0.0;
	bool proposed = use_relative ? this->propose_relative(candidate, q_fwd, q_rev)
	                              : this->propose_direct(candidate, q_fwd, q_rev);

	uint64_t next_page;
	if (!proposed) {
		// Neither kernel has an entry for the current state (e.g. right after
		// seeding, before any training pair was ever seen at direct[cur_page]).
		// Propose directly from the target distribution pi=visits_: an
		// independence proposal that's always accepted, since q(x|y)=pi(x)
		// for every x, y makes the MH ratio exactly 1.
		next_page = this->sample_from_visits();
	} else {
		double pi_cur = (double)this->visits_.at(this->cur_page);
		auto cand_it = this->visits_.find(candidate);
		double pi_cand = (cand_it == this->visits_.end()) ? 0.0 : (double)cand_it->second;

		double accept = (pi_cand > 0.0) ? std::min(1.0, (pi_cand * q_rev) / (pi_cur * q_fwd)) : 0.0;
		double r = this->generate_random_double(&this->seed);
		// Rejected proposals self-loop (repeat cur_page) -- a perfectly
		// normal MH outcome, not an error; it's what keeps the walk's
		// long-run visitation frequency converging to pi.
		next_page = (r < accept) ? candidate : this->cur_page;
	}

	op->type = READ;
	op->key = next_page;
	op->scan_length = 0;
	this->cur_page = next_page;
	++this->cur_nr_op;
	op->is_last_op = !this->has_next_op();
}