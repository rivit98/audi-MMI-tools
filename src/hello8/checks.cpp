// checks.cpp - see checks.h. Each check records the first failed
// expectation (EXPECT) and, while everything passes, a few notes
// (timings, sizes) for the log.
#include "checks.h"

#include <algorithm>
#include <any>
#include <array>
#include <atomic>
#include <bitset>
#include <cctype>
#include <chrono>
#include <climits>
#include <cmath>
#include <complex>
#include <condition_variable>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <exception>
#include <forward_list>
#include <functional>
#include <limits>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <numbers>
#include <numeric>
#include <optional>
#include <queue>
#include <random>
#include <ranges>
#include <set>
#include <shared_mutex>
#include <span>
#include <stack>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <typeinfo>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include <pthread.h>
#include <sched.h>
#include <time.h>

namespace checks {

namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

void expect(Result &r, bool cond, const char *expr, int line)
{
	if (!cond && r.ok) {
		r.ok = false;
		r.detail = "line " + std::to_string(line) + ": " + expr;
	}
}

#define EXPECT(cond) expect(r, static_cast<bool>(cond), #cond, __LINE__)

__attribute__((format(printf, 1, 2))) std::string fmt(const char *f, ...)
{
	char buf[256];
	va_list ap;

	va_start(ap, f);
	vsnprintf(buf, sizeof(buf), f, ap);
	va_end(ap);
	return buf;
}

void note(Result &r, const std::string &s)
{
	if (!r.ok)
		return;
	if (!r.detail.empty())
		r.detail += ", ";
	r.detail += s;
}

unsigned ms_since(Clock::time_point t0)
{
	return (unsigned)std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
}

unsigned us_since(Clock::time_point t0)
{
	return (unsigned)std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - t0).count();
}

// deterministic test data, the same on the car and the host
struct Lcg {
	uint32_t s;
	uint32_t operator()() { s = s * 1664525u + 1013904223u; return s >> 8; }
};

bool near(double a, double b, double eps) { return std::fabs(a - b) <= eps; }

// ---- static initialisation ------------------------------------------------

bool main_started;
int init_seq;

struct Early {
	int order;
	bool before_main;
	Early() : order(++init_seq), before_main(!main_started) {}
};

// same translation unit: constructed in order of definition
Early early_a, early_b;

// dynamic initialisation (not constant-folded: the value comes from a call)
__attribute__((noinline)) int forty_two() { return std::string("forty-two").size() * 4 + 6; }
int dynamic_value = forty_two();

bool ctor_attr_ran;
__attribute__((constructor)) void ctor_attr() { ctor_attr_ran = true; }

int local_static_calls;
int &local_static()
{
	static int v = ++local_static_calls * 7;
	return v;
}

void check_static_init(Result &r)
{
	EXPECT(early_a.order == 1 && early_b.order == 2);
	EXPECT(early_a.before_main && early_b.before_main);
	EXPECT(dynamic_value == 42);
	EXPECT(ctor_attr_ran);
	EXPECT(local_static() == 7);
	local_static() = 8;
	EXPECT(local_static() == 8);
	EXPECT(local_static_calls == 1);
}

// ---- exceptions -------------------------------------------------------------

struct MyError : std::logic_error {
	int code;
	explicit MyError(int c) : std::logic_error("my error"), code(c) {}
};

struct Counted {
	int &n;
	~Counted() { ++n; }
};

__attribute__((noinline)) void deep_throw(int depth, int &unwound)
{
	Counted c{unwound};
	if (depth == 0)
		throw std::out_of_range("bottom");
	deep_throw(depth - 1, unwound);
}

struct UncaughtProbe {
	int &seen;
	~UncaughtProbe() { seen = std::uncaught_exceptions(); }
};

struct ThrowsInCtor {
	Counted a, b;
	explicit ThrowsInCtor(int &n) : a{n}, b{n} { throw 5; }
};

void nothrow_fn() noexcept {}
void may_throw_fn() {}

__attribute__((noinline)) int thrower(int v)
{
	if (v >= 0)
		throw v;
	return v;
}

void check_exceptions(Result &r)
{
	int caught = 0;
	try {
		thrower(42);
	} catch (int v) {
		caught = v;
	}
	EXPECT(caught == 42);

	std::string what;
	try {
		throw std::runtime_error("boom");
	} catch (const std::exception &e) {
		what = e.what();
	}
	EXPECT(what == "boom");

	int code = 0;
	try {
		throw MyError(7);
	} catch (const std::logic_error &e) {
		code = dynamic_cast<const MyError &>(e).code;
	}
	EXPECT(code == 7);

	// destructors of every frame on the way up
	int unwound = 0;
	bool bottom = false;
	try {
		deep_throw(10, unwound);
	} catch (const std::out_of_range &) {
		bottom = true;
	}
	EXPECT(bottom && unwound == 11);

	bool rethrown = false;
	try {
		try {
			throw std::invalid_argument("x");
		} catch (...) {
			throw;
		}
	} catch (const std::invalid_argument &) {
		rethrown = true;
	}
	EXPECT(rethrown);

	std::exception_ptr ep;
	try {
		throw std::length_error("len");
	} catch (...) {
		ep = std::current_exception();
	}
	what.clear();
	try {
		std::rethrow_exception(ep);
	} catch (const std::length_error &e) {
		what = e.what();
	}
	EXPECT(what == "len");
	try {
		std::rethrow_exception(std::make_exception_ptr(MyError(3)));
	} catch (const MyError &e) {
		code = e.code;
	}
	EXPECT(code == 3);

	std::string inner;
	try {
		try {
			throw std::runtime_error("inner");
		} catch (...) {
			std::throw_with_nested(std::logic_error("outer"));
		}
	} catch (const std::logic_error &e) {
		try {
			std::rethrow_if_nested(e);
		} catch (const std::runtime_error &in) {
			inner = in.what();
		}
	}
	EXPECT(inner == "inner");

	int seen = -1;
	try {
		UncaughtProbe p{seen};
		throw 1;
	} catch (int) {
	}
	EXPECT(seen == 1);
	EXPECT(std::uncaught_exceptions() == 0);

	// members already built are destroyed when the constructor throws
	int built = 0;
	try {
		ThrowsInCtor t(built);
	} catch (int) {
	}
	EXPECT(built == 2);

	// through std::function and a lambda
	std::function<void()> f = [] { throw std::overflow_error("lambda"); };
	bool via_function = false;
	try {
		f();
	} catch (const std::overflow_error &) {
		via_function = true;
	}
	EXPECT(via_function);

	EXPECT(noexcept(nothrow_fn()));
	EXPECT(!noexcept(may_throw_fn()));

	// std::vector::at, from inside libstdc++.a
	bool at_threw = false;
	try {
		std::vector<int> v(3);
		(void)v.at(3);
	} catch (const std::out_of_range &) {
		at_threw = true;
	}
	EXPECT(at_threw);

	const int n = 1000;
	int sum = 0;
	auto t0 = Clock::now();
	for (int i = 0; i < n; i++) {
		try {
			thrower(i);
		} catch (int v) {
			sum += v;
		}
	}
	unsigned us = us_since(t0);
	EXPECT(sum == n * (n - 1) / 2);
	note(r, fmt("throw+catch %u us", us / n));
}

// ---- new / delete -------------------------------------------------------------

struct Tracked {
	static int live;
	Tracked() { ++live; }
	~Tracked() { --live; }
};
int Tracked::live;

struct alignas(64) Wide {
	char c[64];
};

// Allocations whose pointer escapes here can't be elided: the compiler may
// drop a new-expression that is only deleted again (C++14 [expr.new]).
void *volatile alloc_sink;

void check_new_delete(Result &r)
{
	int *p = new int[1000];
	for (int i = 0; i < 1000; i++)
		p[i] = i;
	EXPECT(std::accumulate(p, p + 1000, 0) == 499500);
	delete[] p;

	Tracked *a = new Tracked[10];
	EXPECT(Tracked::live == 10);
	delete[] a;
	EXPECT(Tracked::live == 0);

	alignas(Tracked) unsigned char buf[sizeof(Tracked)];
	Tracked *t = new (buf) Tracked;
	EXPECT(Tracked::live == 1 && (void *)t == buf);
	t->~Tracked();
	EXPECT(Tracked::live == 0);

	// C++17 aligned new (operator new(size_t, align_val_t))
	Wide *w = new Wide;
	alloc_sink = w;
	EXPECT(((uintptr_t)w & 63) == 0);
	delete w;

	volatile size_t huge = std::numeric_limits<size_t>::max() / 2;
	void *q = ::operator new(huge, std::nothrow);
	EXPECT(q == nullptr);

	// element count * size overflows: thrown before any allocation
	bool bad_length = false;
	try {
		int *i = new int[huge];
		alloc_sink = i;
		delete[] i;
	} catch (const std::bad_array_new_length &) {
		bad_length = true;
	}
	EXPECT(bad_length);

	// malloc fails. (ASan's operator new returns nullptr instead of throwing,
	// so the host test can't check this one.)
#ifndef __SANITIZE_ADDRESS__
	bool bad_alloc = false;
	try {
		char *c = new char[huge];
		alloc_sink = c;
		delete[] c;
	} catch (const std::bad_alloc &) {
		bad_alloc = true;
	}
	EXPECT(bad_alloc);
#endif
}

// ---- strings ----------------------------------------------------------------

void check_strings(Result &r)
{
	std::string s = "hello";
	s += ", world";
	EXPECT(s == "hello, world" && s.size() == 12);
	EXPECT(s.find("world") == 7);
	EXPECT(s.rfind('o') == 8);
	EXPECT(s.substr(7, 5) == "world");
	EXPECT(s.find("xyz") == std::string::npos);
	s.replace(0, 5, "HELLO");
	EXPECT(s == "HELLO, world");
	EXPECT(s.compare("HELLO") > 0);
	EXPECT(std::string("abc") < std::string("abd"));

	// past the small-string buffer: heap, copy, move
	std::string big(1000, 'x');
	std::string copy = big;
	std::string moved = std::move(copy);
	EXPECT(moved.size() == 1000 && moved == big && moved[999] == 'x');
	big.insert(500, "MIDDLE");
	EXPECT(big.size() == 1006 && big.substr(500, 6) == "MIDDLE");

	EXPECT(std::to_string(-123) == "-123");
	EXPECT(std::to_string(4294967295u) == "4294967295");
	EXPECT(std::to_string(-9223372036854775807LL - 1) == "-9223372036854775808");
	EXPECT(std::to_string(1.5) == "1.500000");          // vsnprintf %f, QNX's libc

	size_t pos = 0;
	EXPECT(std::stoi("  -42abc", &pos) == -42 && pos == 5);
	EXPECT(std::stoi("ff", nullptr, 16) == 255);
	EXPECT(std::stol("2147483647") == 2147483647L);
	EXPECT(std::stoll("-9223372036854775808") == LLONG_MIN);
	EXPECT(std::stoull("18446744073709551615") == ULLONG_MAX);
	EXPECT(std::stod("3.25") == 3.25);
	EXPECT(std::stof("0.5") == 0.5f);

	// errors come back through errno: QNX's, read by libstdc++'s headers
	bool invalid = false, range = false;
	try {
		(void)std::stoi("abc");
	} catch (const std::invalid_argument &) {
		invalid = true;
	}
	try {
		(void)std::stoi("99999999999");
	} catch (const std::out_of_range &) {
		range = true;
	}
	EXPECT(invalid);
	EXPECT(range);

	std::string_view sv = "abcdef";
	EXPECT(sv.substr(2, 3) == "cde");
	EXPECT(sv.starts_with("ab") && sv.ends_with("ef"));
	EXPECT(std::hash<std::string>{}("abc") == std::hash<std::string_view>{}("abc"));

	std::u16string u16 = u"héllo";
	EXPECT(u16.size() == 5);
	std::wstring ws = L"wide";
	ws += L'!';
	EXPECT(ws.size() == 5 && ws.find(L'!') == 4);
}

// ---- vector + algorithms ----------------------------------------------------

void check_algorithms(Result &r)
{
	Lcg rng{1};
	std::vector<int> v;
	for (int i = 0; i < 10000; i++)
		v.push_back((int)(rng() % 100000));
	EXPECT(v.size() == 10000 && v.capacity() >= 10000);

	std::vector<int> sorted = v;
	auto t0 = Clock::now();
	std::sort(sorted.begin(), sorted.end());
	unsigned sort_us = us_since(t0);
	EXPECT(std::is_sorted(sorted.begin(), sorted.end()));
	EXPECT(std::accumulate(v.begin(), v.end(), 0LL) ==
	       std::accumulate(sorted.begin(), sorted.end(), 0LL));
	EXPECT(*std::min_element(v.begin(), v.end()) == sorted.front());
	EXPECT(*std::max_element(v.begin(), v.end()) == sorted.back());

	for (int i = 0; i < 100; i++) {
		int x = v[i * 97];
		EXPECT(std::binary_search(sorted.begin(), sorted.end(), x));
		auto [lo, hi] = std::equal_range(sorted.begin(), sorted.end(), x);
		EXPECT(hi - lo == std::count(v.begin(), v.end(), x));
		EXPECT(std::lower_bound(sorted.begin(), sorted.end(), x) == lo);
		EXPECT(std::upper_bound(sorted.begin(), sorted.end(), x) == hi);
	}

	// stable_sort keeps equal keys in their original order
	std::vector<std::pair<int, int>> st;
	for (int i = 0; i < 1000; i++)
		st.emplace_back((int)(rng() % 10), i);
	std::stable_sort(st.begin(), st.end(),
	                 [](const auto &a, const auto &b) { return a.first < b.first; });
	bool stable = true;
	for (size_t i = 1; i < st.size(); i++)
		if (st[i - 1].first == st[i].first && st[i - 1].second > st[i].second)
			stable = false;
	EXPECT(stable);

	std::vector<int> u = sorted;
	u.erase(std::unique(u.begin(), u.end()), u.end());
	EXPECT(std::adjacent_find(u.begin(), u.end()) == u.end());
	EXPECT(u.size() <= sorted.size());

	std::vector<int> odd = v;
	odd.erase(std::remove_if(odd.begin(), odd.end(), [](int x) { return x % 2 == 0; }), odd.end());
	EXPECT(std::all_of(odd.begin(), odd.end(), [](int x) { return x % 2 == 1; }));
	EXPECT((long)odd.size() == std::count_if(v.begin(), v.end(), [](int x) { return x % 2; }));

	std::vector<int> nth = v;
	std::nth_element(nth.begin(), nth.begin() + 5000, nth.end());
	EXPECT(nth[5000] == sorted[5000]);

	std::vector<int> part = v;
	auto mid = std::partition(part.begin(), part.end(), [](int x) { return x < 50000; });
	EXPECT(std::is_partitioned(part.begin(), part.end(), [](int x) { return x < 50000; }));
	EXPECT(std::none_of(part.begin(), mid, [](int x) { return x >= 50000; }));

	std::vector<int> rot{1, 2, 3, 4, 5};
	std::rotate(rot.begin(), rot.begin() + 2, rot.end());
	EXPECT((rot == std::vector<int>{3, 4, 5, 1, 2}));
	std::reverse(rot.begin(), rot.end());
	EXPECT((rot == std::vector<int>{2, 1, 5, 4, 3}));

	std::vector<int> perm{1, 2, 3, 4};
	int perms = 1;
	while (std::next_permutation(perm.begin(), perm.end()))
		perms++;
	EXPECT(perms == 24);

	std::vector<int> a{1, 3, 5, 7}, b{3, 4, 5, 6}, out;
	std::set_union(a.begin(), a.end(), b.begin(), b.end(), std::back_inserter(out));
	EXPECT((out == std::vector<int>{1, 3, 4, 5, 6, 7}));
	out.clear();
	std::set_intersection(a.begin(), a.end(), b.begin(), b.end(), std::back_inserter(out));
	EXPECT((out == std::vector<int>{3, 5}));
	out.clear();
	std::set_difference(a.begin(), a.end(), b.begin(), b.end(), std::back_inserter(out));
	EXPECT((out == std::vector<int>{1, 7}));

	std::vector<int> heap{3, 1, 4, 1, 5, 9, 2, 6};
	std::make_heap(heap.begin(), heap.end());
	EXPECT(heap.front() == 9);
	heap.push_back(10);
	std::push_heap(heap.begin(), heap.end());
	EXPECT(heap.front() == 10);
	std::pop_heap(heap.begin(), heap.end());
	EXPECT(heap.back() == 10);
	heap.pop_back();
	std::sort_heap(heap.begin(), heap.end());
	EXPECT(std::is_sorted(heap.begin(), heap.end()));

	std::vector<int> shuffled = sorted;
	std::shuffle(shuffled.begin(), shuffled.end(), std::mt19937(1));
	EXPECT(shuffled != sorted);
	std::sort(shuffled.begin(), shuffled.end());
	EXPECT(shuffled == sorted);

	std::vector<int> io(4), ps(4);
	std::iota(io.begin(), io.end(), 1);
	std::partial_sum(io.begin(), io.end(), ps.begin());
	EXPECT((ps == std::vector<int>{1, 3, 6, 10}));
	EXPECT(std::inner_product(io.begin(), io.begin() + 3, io.begin() + 1, 0) == 1 * 2 + 2 * 3 + 3 * 4);
	std::vector<int> sq(4);
	std::transform(io.begin(), io.end(), sq.begin(), [](int x) { return x * x; });
	EXPECT((sq == std::vector<int>{1, 4, 9, 16}));
	EXPECT(std::find_if(sq.begin(), sq.end(), [](int x) { return x > 5; }) - sq.begin() == 2);
	auto [mn, mx] = std::minmax({4, -2, 9, 0});
	EXPECT(mn == -2 && mx == 9);
	EXPECT(std::gcd(12, 18) == 6 && std::lcm(4, 6) == 12);

	std::array<int, 5> arr{5, 4, 3, 2, 1};
	std::sort(arr.begin(), arr.end());
	EXPECT(arr[0] == 1 && arr[4] == 5);

	// C++20 ranges, views, span
	std::vector<int> r20(10);
	std::iota(r20.begin(), r20.end(), 0);
	std::ranges::sort(r20, std::greater{});
	EXPECT(r20.front() == 9);
	int odd_squares = 0;
	for (int x : r20 | std::views::filter([](int x) { return x % 2; })
	              | std::views::transform([](int x) { return x * x; }))
		odd_squares += x;
	EXPECT(odd_squares == 1 + 9 + 25 + 49 + 81);
	std::span<const int> sp(r20.data() + 2, 3);
	EXPECT(sp.size() == 3 && sp[0] == 7);

	std::vector<bool> bits(100);
	bits.flip();
	bits[3] = false;
	EXPECT(std::count(bits.begin(), bits.end(), true) == 99);

	std::vector<std::string> names{"c", "a", "b"};
	names.insert(names.begin() + 1, "z");
	names.erase(names.begin());
	EXPECT((names == std::vector<std::string>{"z", "a", "b"}));

	note(r, fmt("sort 10k %u us", sort_us));
}

// ---- map / set --------------------------------------------------------------

struct CaseLess {
	bool operator()(const std::string &a, const std::string &b) const
	{
		return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(),
		                                    [](char x, char y) { return std::tolower(x) < std::tolower(y); });
	}
};

void check_map_set(Result &r)
{
	std::map<std::string, int> m;
	m["banana"] = 2;
	m["apple"] = 1;
	m.emplace("cherry", 3);
	EXPECT(!m.insert({"apple", 9}).second && m["apple"] == 1);
	std::string keys;
	for (const auto &[k, v] : m)
		keys += k + "=" + std::to_string(v) + " ";
	EXPECT(keys == "apple=1 banana=2 cherry=3 ");
	EXPECT(m.lower_bound("b")->first == "banana");
	EXPECT(m.upper_bound("banana")->first == "cherry");
	EXPECT(m.erase("banana") == 1 && !m.contains("banana"));
	EXPECT(m.find("cherry")->second == 3);
	EXPECT(m.rbegin()->first == "cherry");
	bool at_threw = false;
	try {
		(void)m.at("zzz");
	} catch (const std::out_of_range &) {
		at_threw = true;
	}
	EXPECT(at_threw);
	auto nh = m.extract("apple");
	nh.key() = "avocado";
	m.insert(std::move(nh));
	EXPECT(m.size() == 2 && m.begin()->first == "avocado" && m.begin()->second == 1);

	// 10000 random keys against a sorted, de-duplicated vector
	Lcg rng{7};
	std::map<int, int> big;
	std::vector<int> ref;
	auto t0 = Clock::now();
	for (int i = 0; i < 10000; i++) {
		int k = (int)(rng() % 50000);
		big[k] = i;
		ref.push_back(k);
	}
	unsigned insert_us = us_since(t0);
	std::sort(ref.begin(), ref.end());
	ref.erase(std::unique(ref.begin(), ref.end()), ref.end());
	EXPECT(big.size() == ref.size());
	EXPECT(std::equal(big.begin(), big.end(), ref.begin(),
	                  [](const auto &kv, int k) { return kv.first == k; }));
	std::erase_if(big, [](const auto &kv) { return kv.first % 2 == 0; });
	EXPECT(std::none_of(big.begin(), big.end(), [](const auto &kv) { return kv.first % 2 == 0; }));
	EXPECT((long)big.size() == std::count_if(ref.begin(), ref.end(), [](int k) { return k % 2; }));

	std::multimap<int, char> mm{{1, 'a'}, {2, 'b'}, {1, 'c'}};
	auto [lo, hi] = mm.equal_range(1);
	std::string vals;
	for (auto it = lo; it != hi; ++it)
		vals += it->second;
	EXPECT(vals == "ac");

	std::set<int> s;
	EXPECT(s.insert(3).second && !s.insert(3).second);
	s.insert({1, 5, 4});
	EXPECT(s.size() == 4 && *s.begin() == 1 && *s.rbegin() == 5 && s.contains(4));
	EXPECT(*s.lower_bound(2) == 3);
	std::set<int, std::greater<int>> desc{1, 5, 3};
	EXPECT(*desc.begin() == 5);
	std::multiset<int> ms{2, 1, 2, 2};
	EXPECT(ms.count(2) == 3 && ms.size() == 4);
	std::set<std::string, CaseLess> ci{"Hello", "hello", "WORLD"};
	EXPECT(ci.size() == 2 && ci.contains("world"));

	note(r, fmt("map insert 10k %u us", insert_us));
}

// ---- unordered containers -----------------------------------------------------

struct Point {
	int x, y;
	bool operator==(const Point &) const = default;
};

struct PointHash {
	size_t operator()(const Point &p) const { return std::hash<int>{}(p.x) * 31 + std::hash<int>{}(p.y); }
};

void check_unordered(Result &r)
{
	// keys i * 7919 mod 100003 (a prime): 10000 distinct keys
	std::unordered_map<int, int> u;
	auto t0 = Clock::now();
	for (int i = 0; i < 10000; i++)
		u[(int)((long long)i * 7919 % 100003)] = i;
	unsigned insert_us = us_since(t0);
	EXPECT(u.size() == 10000);
	bool all_found = true;
	for (int i = 0; i < 10000; i++) {
		auto it = u.find((int)((long long)i * 7919 % 100003));
		if (it == u.end() || it->second != i)
			all_found = false;
	}
	EXPECT(all_found);
	EXPECT(u.bucket_count() > 0 && u.load_factor() <= u.max_load_factor());
	u.rehash(50000);                                     // floor() from libm
	EXPECT(u.bucket_count() >= 50000 && u.size() == 10000);
	for (int i = 0; i < 10000; i += 2)
		u.erase((int)((long long)i * 7919 % 100003));
	EXPECT(u.size() == 5000 && !u.contains(0) && u.contains(7919));

	std::unordered_set<std::string> us{"a", "b", "c"};
	EXPECT(us.contains("b") && !us.insert("b").second && us.size() == 3);
	std::unordered_map<Point, int, PointHash> pm{{{1, 2}, 3}, {{2, 1}, 4}};
	EXPECT(pm.at({1, 2}) == 3 && pm.at({2, 1}) == 4);
	std::unordered_multimap<int, int> umm{{1, 1}, {1, 2}, {2, 3}};
	EXPECT(umm.count(1) == 2);

	note(r, fmt("insert 10k %u us", insert_us));
}

// ---- other containers and vocabulary types ------------------------------------

template <class... Ts>
struct overloaded : Ts... {
	using Ts::operator()...;
};

void check_containers(Result &r)
{
	std::list<int> l{5, 1, 4};
	l.sort();
	EXPECT((l == std::list<int>{1, 4, 5}));
	l.push_front(0);
	l.reverse();
	EXPECT(l.front() == 5 && l.back() == 0);
	std::list<int> other{7, 8};
	l.splice(l.begin(), other);
	EXPECT(l.size() == 6 && other.empty() && l.front() == 7);
	l.remove(4);
	EXPECT(l.size() == 5);

	std::forward_list<int> fl{3, 1, 2};
	fl.sort();
	EXPECT(fl.front() == 1);

	std::deque<int> dq;
	for (int i = 0; i < 1000; i++) {
		dq.push_back(i);
		dq.push_front(-i);
	}
	EXPECT(dq.size() == 2000 && dq.front() == -999 && dq.back() == 999 && dq[1000] == 0);
	dq.pop_front();
	EXPECT(dq.front() == -998);

	std::priority_queue<int> pq;
	for (int x : {3, 1, 4, 1, 5, 9})
		pq.push(x);
	std::string order;
	while (!pq.empty()) {
		order += std::to_string(pq.top());
		pq.pop();
	}
	EXPECT(order == "954311");
	std::queue<int> q;
	q.push(1);
	q.push(2);
	EXPECT(q.front() == 1 && q.back() == 2);
	std::stack<int> stk;
	stk.push(1);
	stk.push(2);
	EXPECT(stk.top() == 2);

	std::bitset<64> bs(0xF0F0);
	EXPECT(bs.count() == 8 && bs.test(4) && !bs.test(0));
	EXPECT(bs.to_string().substr(48) == "1111000011110000");

	std::tuple<int, std::string, double> t{1, "two", 3.0};
	auto [ti, ts, td] = t;
	EXPECT(ti == 1 && ts == "two" && td == 3.0 && std::get<1>(t) == "two");
	EXPECT(std::tuple_size_v<decltype(t)> == 3);
	EXPECT(std::apply([](int a, const std::string &b, double c) { return a + (int)b.size() + (int)c; }, t) == 7);
	EXPECT(std::make_pair(1, 2) < std::make_pair(1, 3));

	std::optional<int> o;
	EXPECT(!o && o.value_or(5) == 5);
	o = 3;
	EXPECT(o && *o == 3);
	bool bad_opt = false;
	try {
		std::optional<int> e;
		(void)e.value();
	} catch (const std::bad_optional_access &) {
		bad_opt = true;
	}
	EXPECT(bad_opt);

	std::variant<int, std::string> var = std::string("text");
	EXPECT(var.index() == 1 && std::holds_alternative<std::string>(var));
	EXPECT(std::visit(overloaded{[](int i) { return i; },
	                             [](const std::string &s) { return (int)s.size(); }}, var) == 4);
	EXPECT(std::get_if<int>(&var) == nullptr);
	bool bad_var = false;
	try {
		(void)std::get<int>(var);
	} catch (const std::bad_variant_access &) {
		bad_var = true;
	}
	EXPECT(bad_var);

	std::any any = 42;
	EXPECT(std::any_cast<int>(any) == 42);
	bool bad_any = false;
	try {
		(void)std::any_cast<std::string>(any);
	} catch (const std::bad_any_cast &) {
		bad_any = true;
	}
	EXPECT(bad_any);
	any = std::string("x");
	EXPECT(any.type() == typeid(std::string));
}

// ---- functional ---------------------------------------------------------------

struct Adder {
	int base;
	int add(int x) const { return base + x; }
};

void check_functional(Result &r)
{
	int base = 10;
	auto add = [base](int x) { return base + x; };
	EXPECT(add(5) == 15);
	int count = 0;
	auto inc = [&count] { ++count; };
	inc();
	inc();
	EXPECT(count == 2);
	auto counter = [n = 0]() mutable { return ++n; };
	counter();
	EXPECT(counter() == 2);

	std::function<int(int)> f = add;
	EXPECT(f(1) == 11);
	std::array<int, 64> big{};
	big.fill(1);
	std::function<int(int)> heavy = [big](int x) { return x + std::accumulate(big.begin(), big.end(), 0); };
	EXPECT(heavy(1) == 65);
	std::function<int(int)> copied = heavy;
	EXPECT(copied(0) == 64);
	bool bad_call = false;
	try {
		std::function<void()> empty;
		empty();
	} catch (const std::bad_function_call &) {
		bad_call = true;
	}
	EXPECT(bad_call);

	using namespace std::placeholders;
	auto rminus = std::bind(std::minus<int>(), _2, _1);
	EXPECT(rminus(3, 10) == 7);
	int y = 0;
	std::invoke([](int &x) { x = 5; }, std::ref(y));
	EXPECT(y == 5);
	auto twice = [](auto x) { return x + x; };
	EXPECT(twice(2) == 4 && twice(std::string("ab")) == "abab");
	std::function<int(int)> fib = [&fib](int n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); };
	EXPECT(fib(20) == 6765);
	Adder ad{3};
	EXPECT(std::mem_fn(&Adder::add)(ad, 4) == 7);
	EXPECT(std::hash<int>{}(12345) == std::hash<int>{}(12345));
}

// ---- smart pointers -------------------------------------------------------------

struct Res {
	int &alive;
	explicit Res(int &a) : alive(a) { ++alive; }
	virtual ~Res() { --alive; }
};

struct ResChild : Res {
	using Res::Res;
};

struct Node : std::enable_shared_from_this<Node> {
	int v = 1;
};

void check_smart_pointers(Result &r)
{
	int alive = 0;
	{
		auto u = std::make_unique<Res>(alive);
		EXPECT(alive == 1);
		auto u2 = std::move(u);
		EXPECT(!u && u2);
	}
	EXPECT(alive == 0);

	bool deleted = false;
	{
		std::unique_ptr<int, std::function<void(int *)>> p(new int(3), [&](int *q) {
			deleted = true;
			delete q;
		});
	}
	EXPECT(deleted);
	std::unique_ptr<int[]> arr(new int[10]());
	arr[9] = 1;
	EXPECT(arr[0] == 0 && arr[9] == 1);

	std::weak_ptr<Res> w;
	{
		auto s1 = std::make_shared<Res>(alive);
		EXPECT(s1.use_count() == 1);
		auto s2 = s1;
		EXPECT(s1.use_count() == 2 && alive == 1);
		w = s1;
		EXPECT(!w.expired() && w.lock() == s1);
	}
	EXPECT(w.expired() && w.lock() == nullptr && alive == 0);
	bool bad_weak = false;
	try {
		std::shared_ptr<Res> s(w);
	} catch (const std::bad_weak_ptr &) {
		bad_weak = true;
	}
	EXPECT(bad_weak);

	auto n = std::make_shared<Node>();
	auto n2 = n->shared_from_this();
	EXPECT(n.use_count() == 2 && n2 == n && !n->weak_from_this().expired());

	std::shared_ptr<Res> base = std::make_shared<ResChild>(alive);
	EXPECT(std::dynamic_pointer_cast<ResChild>(base) != nullptr);
	base.reset();
	EXPECT(alive == 0);

	// aliasing: shares ownership of the pair, points at its second member
	auto pair = std::make_shared<std::pair<int, int>>(1, 2);
	std::shared_ptr<int> second(pair, &pair->second);
	EXPECT(*second == 2 && pair.use_count() == 2);

	bool void_deleted = false;
	{
		std::shared_ptr<void> v(new int(1), [&](void *q) {
			void_deleted = true;
			delete static_cast<int *>(q);
		});
	}
	EXPECT(void_deleted);
}

// ---- virtual functions, RTTI ------------------------------------------------------

struct Base {
	static int dtors;
	virtual ~Base() { ++dtors; }
	virtual int f() const { return 1; }
};
int Base::dtors;

struct Mid : Base {
	int f() const override { return 2; }
};

struct Leaf final : Mid {
	static int dtors;
	int f() const override { return 3; }
	~Leaf() override { ++dtors; }
};
int Leaf::dtors;

struct Unrelated {
	virtual ~Unrelated() = default;
};

struct VA {
	virtual ~VA() = default;
	int a = 1;
};
struct VB : virtual VA {
	int b = 2;
};
struct VC : virtual VA {
	int c = 3;
};
struct VD : VB, VC {
	int d = 4;
};

void check_rtti(Result &r)
{
	std::unique_ptr<Base> b = std::make_unique<Leaf>();
	EXPECT(b->f() == 3);
	EXPECT(dynamic_cast<Mid *>(b.get()) != nullptr);
	EXPECT(dynamic_cast<Unrelated *>(b.get()) == nullptr);
	EXPECT(typeid(*b) == typeid(Leaf));
	EXPECT(std::strstr(typeid(Leaf).name(), "Leaf") != nullptr);
	EXPECT(std::strcmp(typeid(int).name(), "i") == 0);

	Mid mid;
	Base &ref = mid;
	bool bad_cast = false;
	try {
		(void)dynamic_cast<Leaf &>(ref);
	} catch (const std::bad_cast &) {
		bad_cast = true;
	}
	EXPECT(bad_cast);

	// virtual inheritance: down-cast and cross-cast
	VD d;
	VA *pa = &d;
	VB *pb = &d;
	EXPECT(dynamic_cast<VD *>(pa) == &d);
	EXPECT(dynamic_cast<VC *>(pb) == static_cast<VC *>(&d));
	EXPECT(pa->a == 1 && d.b == 2 && d.c == 3 && d.d == 4);

	int before = Base::dtors;
	b.reset();
	EXPECT(Base::dtors == before + 1 && Leaf::dtors == 1);
}

// ---- math -------------------------------------------------------------------------

void check_math(Result &r)
{
	using std::numbers::pi;
	EXPECT(near(std::sqrt(2.0), 1.4142135623730951, 1e-15));
	EXPECT(near(std::sqrt(2.0f), 1.41421356f, 1e-6));
	EXPECT(near(std::sin(pi / 6), 0.5, 1e-15));
	EXPECT(near(std::cos(0.0), 1.0, 0));
	EXPECT(near(std::atan2(1.0, 1.0), pi / 4, 1e-15));
	EXPECT(std::pow(2.0, 10) == 1024.0);
	EXPECT(near(std::exp(1.0), std::numbers::e, 1e-15));
	EXPECT(near(std::log(std::numbers::e), 1.0, 1e-15));
	EXPECT(near(std::log10(1000.0), 3.0, 1e-15));
	EXPECT(near(std::log2(8.0), 3.0, 1e-15));
	EXPECT(std::round(2.5) == 3.0 && std::round(-2.5) == -3.0);
	EXPECT(std::trunc(-2.7) == -2.0 && std::floor(-2.5) == -3.0 && std::ceil(2.1) == 3.0);
	EXPECT(std::fmod(7.5, 2.0) == 1.5);
	EXPECT(std::hypot(3.0, 4.0) == 5.0);
	EXPECT(near(std::cbrt(27.0), 3.0, 1e-15));
	EXPECT(std::lround(2.5) == 3);
	EXPECT(std::abs(-3) == 3 && std::abs(-2.5) == 2.5);
	EXPECT(std::isnan(std::nan("")) && std::isinf(HUGE_VAL) && std::isfinite(1.0));
	EXPECT(std::signbit(-0.0) && std::fpclassify(0.0) == FP_ZERO);
	EXPECT(std::numeric_limits<double>::epsilon() == std::ldexp(1.0, -52));
	EXPECT(std::numeric_limits<int>::max() == INT_MAX);
	EXPECT(std::isinf(std::numeric_limits<float>::infinity()));

	std::complex<double> z(3, 4);
	EXPECT(std::abs(z) == 5.0);
	EXPECT(z * std::conj(z) == std::complex<double>(25, 0));
	auto root = std::sqrt(std::complex<double>(-4, 0));
	EXPECT(near(root.real(), 0, 1e-15) && near(root.imag(), 2, 1e-15));
	EXPECT(std::midpoint(1, 3) == 2);
	note(r, fmt("sizeof(long double)=%u", (unsigned)sizeof(long double)));
}

// ---- random -------------------------------------------------------------------------

void check_random(Result &r)
{
	// the 10000th output of a default-seeded engine is fixed by the standard
	std::mt19937 mt;
	mt.discard(9999);
	EXPECT(mt() == 4123659995u);
	std::mt19937_64 mt64;
	mt64.discard(9999);
	EXPECT(mt64() == 9981545732273789042ull);
	std::minstd_rand0 m0;
	m0.discard(9999);
	EXPECT(m0() == 1043618065u);

	std::mt19937 g(2026);
	std::uniform_int_distribution<int> die(1, 6);
	int faces[7] = {};
	for (int i = 0; i < 60000; i++)
		faces[die(g)]++;
	bool fair = true;
	for (int f = 1; f <= 6; f++)
		if (faces[f] < 9400 || faces[f] > 10600)
			fair = false;
	EXPECT(fair && faces[0] == 0);

	std::uniform_real_distribution<double> unit(0.0, 1.0);
	bool in_range = true;
	for (int i = 0; i < 1000; i++) {
		double x = unit(g);
		if (x < 0.0 || x >= 1.0)
			in_range = false;
	}
	EXPECT(in_range);

	std::normal_distribution<double> nd(0.0, 1.0);
	double sum = 0, sum2 = 0;
	const int n = 20000;
	for (int i = 0; i < n; i++) {
		double x = nd(g);
		sum += x;
		sum2 += x * x;
	}
	double mean = sum / n, sd = std::sqrt(sum2 / n - mean * mean);
	EXPECT(near(mean, 0.0, 0.05) && near(sd, 1.0, 0.05));
	note(r, fmt("normal mean %.3f sd %.3f", mean, sd));
}

// ---- chrono ---------------------------------------------------------------------------

void check_chrono(Result &r)
{
	using namespace std::chrono;

	// steady_clock is CLOCK_MONOTONIC, system_clock CLOCK_REALTIME
	struct timespec ts;
	auto steady = steady_clock::now();
	clock_gettime(CLOCK_MONOTONIC, &ts);
	long long mono_ns = (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
	long long diff_ms = (mono_ns - duration_cast<nanoseconds>(steady.time_since_epoch()).count()) / 1000000;
	EXPECT(diff_ms >= 0 && diff_ms < 50);

	auto sys_s = duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
	long long now_s = (long long)time(nullptr);
	EXPECT(sys_s - now_s >= -2 && sys_s - now_s <= 2);
	EXPECT(steady_clock::is_steady);

	auto t0 = steady_clock::now();
	clock_gettime(CLOCK_MONOTONIC, &ts);
	long long c0 = (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
	std::this_thread::sleep_for(100ms);
	clock_gettime(CLOCK_MONOTONIC, &ts);
	long long c1 = (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
	auto slept = duration_cast<milliseconds>(steady_clock::now() - t0).count();
	EXPECT(slept >= 95 && slept < 400);
	EXPECT(c1 - c0 >= 95 && c1 - c0 < 400);
	EXPECT(steady_clock::now() >= t0);

	EXPECT(duration_cast<milliseconds>(2s + 500ms).count() == 2500);
	EXPECT(1h == 3600s && duration<double>(1500ms).count() == 1.5);

	// C++20 calendar
	EXPECT(weekday{sys_days{2026y / October / 22}} == Thursday);
	EXPECT((sys_days{2026y / March / 1} - sys_days{2026y / February / 1}).count() == 28);
	note(r, fmt("sleep_for(100ms) %lld ms", (long long)slept));
}

// ---- threads ----------------------------------------------------------------------------

std::atomic<int> slow_inits;

struct Slow {
	Slow()
	{
		std::this_thread::sleep_for(20ms);
		slow_inits++;
	}
};

void use_slow()
{
	static Slow s;
	(void)s;
}

void check_threads(Result &r)
{
	unsigned hc = std::thread::hardware_concurrency();
	EXPECT(hc >= 1);

	const int nthreads = 4, iter = 20000;
	std::mutex m;
	long locked = 0;
	std::vector<std::thread::id> ids(nthreads);
	std::vector<std::thread> pool;
	auto t0 = Clock::now();
	for (int k = 0; k < nthreads; k++)
		pool.emplace_back([&, k] {
			ids[k] = std::this_thread::get_id();
			for (int i = 0; i < iter; i++) {
				std::lock_guard<std::mutex> g(m);
				++locked;
			}
		});
	for (auto &t : pool)
		t.join();
	unsigned mutex_ms = ms_since(t0);
	EXPECT(locked == (long)nthreads * iter);
	std::set<std::thread::id> distinct(ids.begin(), ids.end());
	EXPECT(distinct.size() == (size_t)nthreads && !distinct.contains(std::this_thread::get_id()));

	int out = 0;
	std::thread args([](int a, std::string s, int &o) { o = a + (int)s.size(); }, 5, std::string("abc"), std::ref(out));
	args.join();
	EXPECT(out == 8 && !args.joinable());

	// exceptions in four threads at once: each its own __cxa_get_globals
	std::atomic<int> wrong{0};
	pool.clear();
	for (int k = 0; k < nthreads; k++)
		pool.emplace_back([k, &wrong] {
			for (int i = 0; i < 500; i++) {
				try {
					thrower(k * 1000 + i);
				} catch (int v) {
					if (v != k * 1000 + i)
						wrong++;
				}
				if (i % 50 == 0)
					std::this_thread::yield();
			}
			if (std::uncaught_exceptions() != 0)
				wrong++;
		});
	for (auto &t : pool)
		t.join();
	EXPECT(wrong == 0);

	// one function-local static, four threads arriving together
	std::atomic<int> ready{0};
	pool.clear();
	for (int k = 0; k < nthreads; k++)
		pool.emplace_back([&ready] {
			ready++;
			while (ready < 4)
				std::this_thread::yield();
			use_slow();
		});
	for (auto &t : pool)
		t.join();
	EXPECT(slow_inits == 1);

	std::recursive_mutex rm;
	rm.lock();
	EXPECT(rm.try_lock());
	rm.unlock();
	rm.unlock();

	// try_lock from the owning thread is undefined: ask another one
	auto try_elsewhere = [](auto &mtx) {
		bool got = false;
		std::thread([&] {
			got = mtx.try_lock();
			if (got)
				mtx.unlock();
		}).join();
		return got;
	};
	std::mutex m1, m2;
	{
		std::scoped_lock both(m1, m2);
		EXPECT(!try_elsewhere(m1) && !try_elsewhere(m2));
	}
	EXPECT(try_elsewhere(m1) && try_elsewhere(m2));

	std::shared_mutex sm;
	{
		std::shared_lock a(sm), b(sm, std::try_to_lock);
		EXPECT(b.owns_lock());
		EXPECT(!try_elsewhere(sm));
	}
	EXPECT(try_elsewhere(sm));

	note(r, fmt("hardware_concurrency %u, 4x%d mutex locks %u ms", hc, iter, mutex_ms));
}

// ---- condition variables, call_once ----------------------------------------------------

void check_sync(Result &r)
{
	// producer / consumer through a condition variable
	std::mutex m;
	std::condition_variable cv;
	std::queue<int> q;
	bool done = false;
	long long consumed = 0;
	std::thread consumer([&] {
		std::unique_lock<std::mutex> lk(m);
		for (;;) {
			cv.wait(lk, [&] { return !q.empty() || done; });
			while (!q.empty()) {
				consumed += q.front();
				q.pop();
			}
			if (done)
				break;
		}
	});
	for (int i = 1; i <= 1000; i++) {
		{
			std::lock_guard<std::mutex> g(m);
			q.push(i);
		}
		cv.notify_one();
	}
	{
		std::lock_guard<std::mutex> g(m);
		done = true;
	}
	cv.notify_all();
	consumer.join();
	EXPECT(consumed == 500500);

	// a timed wait that nobody wakes (the predicate rides out spurious
	// wake-ups; false = timed out)
	std::unique_lock<std::mutex> lk(m);
	auto t0 = Clock::now();
	bool woken = cv.wait_for(lk, 50ms, [] { return false; });
	unsigned waited = ms_since(t0);
	EXPECT(!woken && waited >= 45 && waited < 1000);
	lk.unlock();

	std::once_flag once;
	std::atomic<int> once_runs{0};
	std::vector<std::thread> pool;
	for (int k = 0; k < 4; k++)
		pool.emplace_back([&] { std::call_once(once, [&] { once_runs++; }); });
	for (auto &t : pool)
		t.join();
	EXPECT(once_runs == 1);
	note(r, fmt("wait_for(50ms) %u ms", waited));
}

// ---- sync objects re-created at the same address -----------------------------------------
//
// QNX keeps a kernel sync object per address from a mutex's first lock until
// pthread_mutex_destroy. With libstdc++'s default (PTHREAD_MUTEX_INITIALIZER,
// trivial destructor) the next mutex at that address - the same stack slot,
// a reused heap block - failed its first lock with EINVAL: hello9's second
// HwStream on 2026-11-05. The SDK now destroys (include-c++/pre/bits/
// os_defines.h); here each kind is built, used from two threads, destroyed
// and built again in the same buffer, a few rounds.

template <typename M> std::string reuse_rounds(const char *what)
{
	alignas(M) unsigned char buf[sizeof(M)];
	for (int round = 0; round < 3; round++) {
		M *m = new (buf) M();
		try {
			std::atomic<int> in{0};
			std::thread other([&] {
				std::lock_guard<M> g(*m);
				in++;
			});
			{
				std::lock_guard<M> g(*m);
				in++;
			}
			other.join();
			if (in != 2) {
				m->~M();
				return fmt("%s round %d: %d locks", what, round, in.load());
			}
		} catch (const std::system_error &e) {
			m->~M();
			return fmt("%s round %d: %s (%d)", what, round, e.what(), e.code().value());
		}
		m->~M();
	}
	return "";
}

std::string cv_reuse_rounds()
{
	alignas(std::condition_variable) unsigned char buf[sizeof(std::condition_variable)];
	std::mutex m;
	for (int round = 0; round < 3; round++) {
		auto *cv = new (buf) std::condition_variable();
		bool flag = false;
		try {
			std::thread other([&] {
				std::lock_guard<std::mutex> g(m);
				flag = true;
				cv->notify_one();
			});
			{
				std::unique_lock<std::mutex> lk(m);
				cv->wait_for(lk, 2s, [&] { return flag; });
			}
			other.join();
		} catch (const std::system_error &e) {
			cv->~condition_variable();
			return fmt("condition_variable round %d: %s (%d)", round, e.what(), e.code().value());
		}
		cv->~condition_variable();
		if (!flag)
			return fmt("condition_variable round %d: not woken", round);
	}
	return "";
}

// a class holding a mutex, made in the same stack frame twice (hello9's case)
struct Holder {
	std::mutex mu;
	int n = 0;
	void bump()
	{
		std::lock_guard<std::mutex> g(mu);
		n++;
	}
};

__attribute__((noinline)) std::string holder_once()
{
	Holder h;
	try {
		std::thread t([&] { h.bump(); });
		h.bump();
		t.join();
	} catch (const std::system_error &e) {
		return fmt("Holder on the stack: %s (%d)", e.what(), e.code().value());
	}
	return h.n == 2 ? "" : "Holder on the stack: lost a lock";
}

void check_sync_reuse(Result &r)
{
	for (std::string err : {reuse_rounds<std::mutex>("mutex"),
	                        reuse_rounds<std::recursive_mutex>("recursive_mutex"),
	                        reuse_rounds<std::timed_mutex>("timed_mutex"), cv_reuse_rounds(),
	                        holder_once(), holder_once(), holder_once()}) {
		if (!err.empty()) {
			r.ok = false;
			r.detail = err;
			return;
		}
	}
	// the same in heap blocks: freed and allocated again (often the same block)
	for (int round = 0; round < 3; round++) {
		auto h = std::make_unique<Holder>();
		try {
			std::thread t([&] { h->bump(); });
			h->bump();
			t.join();
		} catch (const std::system_error &e) {
			r.ok = false;
			r.detail = fmt("Holder on the heap round %d: %s (%d)", round, e.what(), e.code().value());
			return;
		}
		EXPECT(h->n == 2);
	}
	note(r, "mutex, recursive_mutex, timed_mutex, condition_variable x3; Holder stack x3, heap x3");
}

// ---- atomics ---------------------------------------------------------------------------

void check_atomics(Result &r)
{
	std::atomic<int> a{5};
	EXPECT(a.exchange(7) == 5 && a == 7);
	int expected = 7;
	EXPECT(a.compare_exchange_strong(expected, 9) && a == 9);
	EXPECT(!a.compare_exchange_strong(expected, 11) && expected == 9);
	EXPECT(a.fetch_add(1) == 9 && a.fetch_sub(2) == 10 && a == 8);
	EXPECT((a.fetch_or(0x10) | 0x10) == 0x18 && (a & 0x10) != 0);
	std::atomic_flag flag;
	EXPECT(!flag.test_and_set() && flag.test_and_set());
	flag.clear();
	std::atomic<bool> b{false};
	b.store(true);
	EXPECT(b.load());

	// 4 threads, each fetch_add 200000 times. Lost updates are counted, not
	// just failed. Too short to interleave (each thread finishes inside one
	// timeslice): "atomics stress" below is the real test.
	const int nthreads = 4, iter = 200000;
	std::atomic<int> count{0};
	std::vector<std::thread> pool;
	auto t0 = Clock::now();
	for (int k = 0; k < nthreads; k++)
		pool.emplace_back([&] {
			for (int i = 0; i < iter; i++)
				count.fetch_add(1, std::memory_order_relaxed);
		});
	for (auto &t : pool)
		t.join();
	unsigned ms = ms_since(t0);
	int lost = nthreads * iter - count.load();
	if (lost != 0 && r.ok) {
		r.ok = false;
		r.detail = fmt("lost %d of %d fetch_adds", lost, nthreads * iter);
	}
	note(r, fmt("4x%d fetch_add %u ms", iter, ms));
}

// ---- atomics stress ----------------------------------------------------------------------
// "atomics" above can't catch the gUSA problem: its threads finish inside one
// timeslice, so they never interleave. Here 4 round-robin threads add to one
// counter for 1.5 s, switched mid-loop hundreds of times, three ways:
//   plain  load + add + store: the control. It MUST lose updates, or the
//          threads didn't interleave and the other two numbers say nothing.
//   gUSA   __atomic_fetch_add in gusa.cpp, built with -matomic-model=soft-gusa,
//          GCC's sh4 default. Lost updates here = QNX doesn't restart the
//          sequence (2026-10-30: 9 % lost).
//   LL/SC  the same in llcs.cpp, with the SDK's default since 2026-10-30,
//          hard-llcs: the fix. It must lose none.
// The counter is 32-bit and wraps (the host's plain loop can exceed 2^32);
// lost = done - counter, mod 2^32.

using AddLoop = unsigned long long (*)(unsigned *count, const bool *stop);

__attribute__((noinline)) unsigned long long plain_add_until(unsigned *count, const bool *stop)
{
	unsigned long long n = 0;

	while (!__atomic_load_n(stop, __ATOMIC_RELAXED)) {
		__atomic_store_n(count, __atomic_load_n(count, __ATOMIC_RELAXED) + 1u,
		                 __ATOMIC_RELAXED);
		n++;
	}
	return n;
}

// Round-robin at the thread's own priority, so equal threads take turns
// every timeslice (QNX: 4 ticks). The host may refuse (Linux needs
// privileges); its scheduler interleaves anyway.
bool round_robin()
{
	sched_param sp;
	int policy;

	if (pthread_getschedparam(pthread_self(), &policy, &sp) != 0)
		return false;
	return policy == SCHED_RR || pthread_setschedparam(pthread_self(), SCHED_RR, &sp) == 0;
}

struct Stress {
	unsigned long long done = 0;
	unsigned lost = 0;
	bool rr = true;
};

Stress stress(AddLoop loop, unsigned ms)
{
	const int nthreads = 4;
	unsigned count = 0;
	bool stop = false;
	unsigned long long done[nthreads] = {};
	std::atomic<bool> all_rr{true};
	std::vector<std::thread> pool;

	for (int k = 0; k < nthreads; k++)
		pool.emplace_back([&, k] {
			if (!round_robin())
				all_rr.store(false);
			done[k] = loop(&count, &stop);
		});
	std::this_thread::sleep_for(std::chrono::milliseconds(ms));
	__atomic_store_n(&stop, true, __ATOMIC_RELAXED);
	for (auto &t : pool)
		t.join();

	Stress s;
	for (unsigned long long d : done)
		s.done += d;
	s.lost = (unsigned)s.done - __atomic_load_n(&count, __ATOMIC_RELAXED);
	s.rr = all_rr.load();
	return s;
}

void check_atomics_stress(Result &r)
{
	const unsigned ms = 1500;
	Stress p = stress(plain_add_until, ms);
	Stress g = stress(gusa_add_until, ms);
	Stress l = stress(llcs_add_until, ms);

	std::string counts = fmt("lost: plain %u, gUSA %u, LL/SC %u; adds %uk/%uk/%uk in %u ms each, %s",
	                         p.lost, g.lost, l.lost, (unsigned)(p.done / 1000),
	                         (unsigned)(g.done / 1000), (unsigned)(l.done / 1000), ms,
	                         p.rr && g.rr && l.rr ? "SCHED_RR" : "default policy");
	if (p.lost == 0) {
		r.ok = false;
		r.detail = "no interleaving (plain lost 0), inconclusive; " + counts;
	} else if (l.lost != 0) {
		r.ok = false;
		r.detail = "LL/SC lost updates; " + counts;
	} else {
		note(r, counts);
	}
}

const std::vector<Check> list = {
	{"static init", check_static_init},
	{"exceptions", check_exceptions},
	{"new/delete", check_new_delete},
	{"strings", check_strings},
	{"algorithms", check_algorithms},
	{"map/set", check_map_set},
	{"unordered", check_unordered},
	{"containers", check_containers},
	{"functional", check_functional},
	{"smart pointers", check_smart_pointers},
	{"virtual/RTTI", check_rtti},
	{"math", check_math},
	{"random", check_random},
	{"chrono", check_chrono},
	{"threads", check_threads},
	{"sync", check_sync},
	{"sync reuse", check_sync_reuse},
	{"atomics", check_atomics},
	{"atomics stress", check_atomics_stress},
};

} // namespace

const std::vector<Check> &all()
{
	return list;
}

Result run(const Check &c)
{
	Result r;
	r.name = c.name;
	auto t0 = Clock::now();
	try {
		c.run(r);
	} catch (const std::exception &e) {
		r.ok = false;
		r.detail = std::string("exception escaped: ") + e.what();
	} catch (...) {
		r.ok = false;
		r.detail = "unknown exception escaped";
	}
	r.ms = ms_since(t0);
	return r;
}

void mark_main()
{
	main_started = true;
}

} // namespace checks
