module;

#if !defined(__cpp_impl_reflection) || __cpp_impl_reflection < 202506L
#error "fmp requires the P2996R13 reflection implementation (__cpp_impl_reflection >= 202506L)"
#endif

export module fmp;

import std;

export namespace fmp {
namespace m = std::meta;

// ── core aliases ──────────────────────────────────────────────────────────

using mi  = m::info;
using sz  = std::size_t;
using sv  = std::string_view;
using u8  = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

template <class T>       using vec     = std::vector<T>;
template <class T>       using opt     = std::optional<T>;
template <class T, sz N> using arr     = std::array<T, N>;
template <class T>       using cvr     = std::remove_cvref_t<T>;
template <sz I>          using ic      = std::integral_constant<sz, I>;
template <sz N>          using mkidx   = std::make_index_sequence<N>;
template <sz... Is>      using idx_seq = std::index_sequence<Is...>;

using mis = vec<mi>;

template <class T, class... Ts> inline constexpr bool one_of_v          = (std::same_as<cvr<T>, Ts> || ...);
template <class...>             inline constexpr bool dependent_false_v = false;

template <class... Fs> struct overload : Fs... { using Fs::operator()...; };
template <class... Fs> overload(Fs...) -> overload<Fs...>;

inline constexpr sz npos = static_cast<sz>(-1);

enum class fail_action : u8 { ignore, debug_assert, reject, clamp, throw_ };
struct check_policy { fail_action action = fail_action::reject; bool diagnose_all = false; };

// ── compile-time strings & forward decls ──────────────────────────────────

template <sz N> struct fixed_string {
	char v[N]{};
	constexpr fixed_string(char const (&s)[N]) { std::copy_n(s, N, v); }
	constexpr sv svv() const { return {v, N - 1}; }
};
template <sz N> fixed_string(char const (&)[N]) -> fixed_string<N>;

template <class T, fixed_string Key> consteval bool has_field();
template <class T, fixed_string Key> consteval mi   field_by_key();

// ── annotations ───────────────────────────────────────────────────────────

namespace ann {
template <sz Capacity = 256> struct text {
	char data[Capacity + 1]{};
	sz   length{};

	constexpr text() = default;
	template <sz N> constexpr text(char const (&s)[N]) : length(N - 1) {
		static_assert(N <= Capacity + 1, "fmp annotation text is too long");
		std::copy_n(s, N, data);
	}
	constexpr explicit text(sv s) : length(std::min(s.size(), Capacity)) { if (length) std::copy_n(s.data(), length, data); }
	constexpr sv view() const { return {data, length}; }
};

struct required    {};
struct defaulted   {};
struct transient   {};
struct id          {};
struct finite      {};
struct nonempty    {};
struct no_validate {};

#define FMP_TEXT_ANN(Name) \
	struct Name { text<> value{}; constexpr Name() = default; constexpr explicit Name(text<> v) : value{v} {} template <sz N> constexpr Name(char const (&s)[N]) : value{s} {} constexpr sv view() const { return value.view(); } }
FMP_TEXT_ANN(key);
FMP_TEXT_ANN(unit);
FMP_TEXT_ANN(label);
#undef FMP_TEXT_ANN

template <auto V> struct min      { static constexpr auto value = V; };
template <auto V> struct max      { static constexpr auto value = V; };
template <auto V> struct gt       { static constexpr auto value = V; };
template <auto V> struct lt       { static constexpr auto value = V; };
template <auto V> struct multiple { static constexpr auto value = V; };
template <sz   N> struct max_len  { static constexpr sz   value = N; };

//  opt-in type annotation: presence enables automatic validation in fmp transitions.
template <fail_action Action = fail_action::throw_, bool DiagnoseAll = false> struct validate {
	static constexpr fail_action action       = Action;
	static constexpr bool        diagnose_all = DiagnoseAll;
};

// field predicate: bool(V const&) or bool(Token, V const&). Type predicate: bool(T const&).
template <auto Fn, sz Capacity = 128> struct check {
	text<Capacity>        message{};
	static constexpr auto fn = Fn;
	constexpr check() = default;
	template <sz N> constexpr check(char const (&s)[N]) : message{s} {}
	constexpr sv view() const noexcept { return message.view(); }
};
} // namespace ann

using ann_text = ann::text<>;

// ── storage hints ─────────────────────────────────────────────────────────

namespace hint {
struct hot      {};
struct cold     {};
struct columnar {};
struct no_wire  {};
template <sz N> struct align { static constexpr sz value = N; };
} // namespace hint

// ── reflection helpers ────────────────────────────────────────────────────

namespace d {
using mio = m::data_member_options;

consteval mis fields(mi r)                { return m::nonstatic_data_members_of(r, m::access_context::current()); }
template <class T> consteval mis fields() { return fields(^^T); }

consteval sv id   (mi x) { return m::identifier_of(x); }
consteval mi ty   (mi x) { return m::dealias(m::type_of(x)); }
consteval mi ann_t(mi a) { return m::dealias(m::remove_cvref(m::type_of(a))); }

consteval mi dm     (mi t,   mio opts) { return m::data_member_spec(t, opts); }
consteval mi dm_like(mi src, mio opts) { opts.annotations = m::annotations_of(src); return m::data_member_spec(ty(src), opts); }

template <class R> consteval mi def(mi t, R &&r) { return m::define_aggregate(t, std::forward<R>(r)); }

template <class A> consteval bool   has_ann  (mi x)                          { return !m::annotations_of_with_type(x, ^^A).empty(); }
template <class A> consteval opt<A> first_ann(mi x)                          { auto xs = m::annotations_of_with_type(x, ^^A); if (xs.empty()) return std::nullopt;                                  return m::extract<A>(xs.front()); }
template <class A> consteval opt<A> only_ann (mi x, char const *duplicate)   { auto xs = m::annotations_of_with_type(x, ^^A); if (xs.size() > 1) throw duplicate; if (xs.empty()) return std::nullopt; return m::extract<A>(xs.front()); }

// re-query annotations per index — workaround for GCC 16.1.1 ICE on expansion-statements over define_static_array.
// thought this was fixed in trunk, but isnt :(
consteval sz annotation_count(mi item)       { return m::annotations_of(item).size(); }
consteval mi annotation_at   (mi item, sz i) { auto xs = m::annotations_of(item); if (i >= xs.size()) throw "fmp: annotation index out of range"; return xs[i]; }

template <sz N> consteval auto index_array() { arr<sz, N> out{}; for (sz i = 0; i != N; ++i) out[i] = i; return out; }

template <class T> consteval bool schema_ok() {
	vec<sv> seen;
	for (mi f : fields<T>()) {
		if (!m::has_identifier(f) || m::is_bit_field(f)) return false;
		sv name = id(f);
		if (std::ranges::contains(seen, name)) return false;
		seen.push_back(name);
	}
	return true;
}

template <sz N, class F> constexpr void           for_seq (F &&f) {        [&]<sz... Is>(idx_seq<Is...>)                   { (std::invoke(f, ic<Is>{}), ...); }                                       (mkidx<N>{}); }
template <sz N, class F> constexpr decltype(auto) with_seq(F &&f) { return [&]<sz... Is>(idx_seq<Is...>) -> decltype(auto) { return std::invoke(std::forward<F>(f), idx_seq<Is...>{}); }(mkidx<N>{}); }

template <sz Lo, sz Hi, class F> constexpr decltype(auto) wi(sz i, F &&f) {
	static_assert(Lo < Hi);
	if constexpr (Lo + 1 == Hi) return std::invoke(std::forward<F>(f), ic<Lo>{});
	else { if (i == Lo) return std::invoke(std::forward<F>(f), ic<Lo>{}); return wi<Lo + 1, Hi>(i, std::forward<F>(f)); }
}
} // namespace d

template <sz N, class F> constexpr decltype(auto) with_index(sz i, F &&f) { if (i >= N) throw std::out_of_range("fmp::with_index"); return d::wi<0, N>(i, std::forward<F>(f)); }

template <class T> concept model = std::is_class_v<T> && d::schema_ok<T>();

// ── field access ──────────────────────────────────────────────────────────

template <class T> consteval         sz nfields()                  { return d::fields<T>().size(); }
template <class T> inline constexpr  sz nfields_v = nfields<T>();

template <class T, sz I>              consteval mi   member_info       () { auto fs = d::fields<T>(); if (I >= fs.size()) throw "fmp: field index out of range"; return fs[I]; }
template <class T, fixed_string Name> consteval bool has_declared_field() { for (mi f : d::fields<T>()) if (m::has_identifier(f) && d::id(f) == Name.svv()) return true; return false; }
template <class T, fixed_string Name> consteval mi   declared_field    () { for (mi f : d::fields<T>()) if (m::has_identifier(f) && d::id(f) == Name.svv()) return f;    throw "fmp: no such declared field"; }

template <class T, sz I>              using field_type_t       = [:m::type_of(member_info<T, I>()):];
template <class T, fixed_string Name> using named_field_type_t = [:m::type_of(declared_field<T, Name>()):];

template <sz I,              class T> constexpr decltype(auto) get(T &&x) noexcept { using U = cvr<T>; static_assert(I < nfields<U>());     return (std::forward<T>(x).[:member_info<U, I>():]); }
template <fixed_string Name, class T> constexpr decltype(auto) get(T &&x) noexcept { using U = cvr<T>; static_assert(has_field<U, Name>()); return (std::forward<T>(x).[:field_by_key<U, Name>():]); }

template <class T, class F> constexpr void each_field(T &&x, F &&f) {
	d::for_seq<nfields_v<cvr<T>>>([&]<sz I>(ic<I> tag) { std::invoke(f, tag, fmp::get<I>(std::forward<T>(x))); });
}

// ── hashing ───────────────────────────────────────────────────────────────

inline constexpr u64 fnv_offset = 14695981039346656037ull;
inline constexpr u64 fnv_prime  = 1099511628211ull;

consteval u64 hash_byte(u64 h, unsigned char c) { return (h ^ c) * fnv_prime; }
consteval u64 hash_sv  (u64 h, sv s)            { for (char c : s) h = hash_byte(h, static_cast<unsigned char>(c)); return h; }
consteval u64 hash_u64 (u64 h, u64 x)           { for (int i = 0; i != 8; ++i) { h = hash_byte(h, static_cast<unsigned char>(x & 0xffu)); x >>= 8; } return h; }

template <class T> consteval u64 semantic_type_hash();

enum class fingerprint_domain : u8 { semantic, layout, storage };

// ── canonical schema atoms ────────────────────────────────────────────────

struct field_atom {
	mi       member{}, type{};
	sv       declared_name{};
	ann_text key{}, unit{}, label{};
	bool     required{}, defaulted{}, transient{}, id{};
	bool     hint_hot{}, hint_cold{}, hint_columnar{}, hint_no_wire{};
	u64      type_hash{}, structural_hash{}, layout_hash{};
};

namespace d {
template <class A> consteval ann_text text_ann_or(mi f, ann_text dflt = {}) { if (auto v = first_ann<A>(f)) return v->value; return dflt; }

consteval u64 type_hash_from_info(mi t) { return hash_sv(fnv_offset, m::display_string_of(t)); }

consteval field_atom atom_of(mi f) {
	field_atom a{};
	a.member        = f;
	a.type          = ty(f);
	a.declared_name = id(f);
	a.key           = text_ann_or<ann::key>  (f, ann_text{id(f)});
	a.unit          = text_ann_or<ann::unit> (f);
	a.label         = text_ann_or<ann::label>(f);
	a.required      = has_ann<ann::required> (f);
	a.defaulted     = has_ann<ann::defaulted>(f);
	a.transient     = has_ann<ann::transient>(f);
	a.id            = has_ann<ann::id>       (f);
	a.hint_hot      = has_ann<hint::hot>     (f);
	a.hint_cold     = has_ann<hint::cold>    (f);
	a.hint_columnar = has_ann<hint::columnar>(f);
	a.hint_no_wire  = has_ann<hint::no_wire> (f);
	a.type_hash     = type_hash_from_info(a.type);

	u64 h = fnv_offset;
	h = hash_sv (h, a.key.view());
	h = hash_u64(h, a.type_hash);
	h = hash_sv (h, a.unit.view());
	for (bool b : {a.required, a.defaulted, a.transient, a.id}) h = hash_byte(h, b ? 1 : 0);
	for (mi an : m::annotations_of(f)) {
		mi at = ann_t(an);
		if (at == ^^ann::label || at == ^^hint::hot || at == ^^hint::cold || at == ^^hint::columnar || at == ^^hint::no_wire) continue;
		h = hash_sv(h, m::display_string_of(at));
	}
	a.structural_hash = h;
	a.layout_hash     = hash_u64(hash_u64(h, m::size_of(f)), m::alignment_of(f));
	return a;
}

template <class T> consteval vec<field_atom> atoms() {
	vec<field_atom> out;
	vec<ann_text>   keys;
	for (mi f : fields<T>()) {
		auto a = atom_of(f);
		if (std::ranges::contains(keys, a.key.view(), [](auto const &k) { return k.view(); }))
			throw "fmp: duplicate semantic field key";
		keys.push_back(a.key);
		out.push_back(a);
	}
	return out;
}

consteval bool same_type(mi a, mi b) { return m::dealias(a) == m::dealias(b); }
} // namespace d

template <class T> consteval vec<field_atom> atoms() { static_assert(model<T>); return d::atoms<T>(); }

// ── field info / tag ──────────────────────────────────────────────────────

struct field_info {
	sz       index{};
	ann_text declared_name{}, key{}, unit{}, label{};
	bool     required{}, defaulted{}, transient{}, id{};
	bool     hint_hot{}, hint_cold{}, hint_columnar{}, hint_no_wire{};
	u64      type_hash{}, structural_hash{}, layout_hash{};

	constexpr sv   name      () const noexcept { return declared_name.view(); }
	constexpr sv   key_view  () const noexcept { return key  .view(); }
	constexpr sv   unit_view () const noexcept { return unit .view(); }
	constexpr sv   label_view() const noexcept { return label.view(); }
	constexpr bool wire      () const noexcept { return !(transient || hint_no_wire); }
};

namespace d {
template <class T, sz I> consteval field_info meta_at() {
	auto a = atoms<T>()[I];
	return {
		.index           = I,
		.declared_name   = ann_text{a.declared_name},
		.key             = a.key,      .unit          = a.unit,          .label         = a.label,
		.required        = a.required, .defaulted     = a.defaulted,     .transient     = a.transient, .id = a.id,
		.hint_hot        = a.hint_hot, .hint_cold     = a.hint_cold,     .hint_columnar = a.hint_columnar, .hint_no_wire = a.hint_no_wire,
		.type_hash       = a.type_hash,
		.structural_hash = a.structural_hash,
		.layout_hash     = a.layout_hash,
	};
}
} // namespace d

template <class T> consteval auto fields() {
	static_assert(model<T>);
	return d::with_seq<nfields<T>()>([]<sz... Is>(idx_seq<Is...>) { return arr<field_info, sizeof...(Is)>{d::meta_at<T, Is>()...}; });
}
template <class T> inline constexpr auto fields_v = fields<T>();

template <class T, sz I> consteval ann_text key_at()  { static_assert(I < nfields<T>()); return fields_v<T>[I].key; }
template <class T, sz I> consteval ann_text name_at() { static_assert(I < nfields<T>()); return fields_v<T>[I].declared_name; }

template <class T, sz I> struct field_tag {
	static_assert(I < nfields<T>());
	static constexpr sz         index = I;
	static constexpr field_info meta  = fields_v<T>[I];

	constexpr sv   name()  const noexcept { return meta.name(); }
	constexpr sv   key()   const noexcept { return meta.key_view(); }
	constexpr sv   unit()  const noexcept { return meta.unit_view(); }
	constexpr sv   label() const noexcept { return meta.label_view(); }
	constexpr bool wire()  const noexcept { return meta.wire(); }
};

template <class T, class F> constexpr void each_meta(T &&x, F &&f) {
	using U = cvr<T>;
	d::for_seq<nfields_v<U>>([&]<sz I>(ic<I>) { std::invoke(f, field_tag<U, I>{}, fmp::get<I>(std::forward<T>(x))); });
}

template <class T, fixed_string Key> consteval bool has_field()    { for (auto const &a : atoms<T>())                                  if (a.key.view()     == Key.svv() || a.declared_name     == Key.svv()) return true; return false; }
template <class T, fixed_string Key> consteval sz   field_index()  { auto as = atoms<T>(); for (sz i = 0; i != as.size(); ++i) if (as[i].key.view() == Key.svv() || as[i].declared_name == Key.svv()) return i;    throw "fmp: no such semantic field"; }
template <class T, fixed_string Key> consteval mi   field_by_key() { return atoms<T>()[field_index<T, Key>()].member; }
template <class T, fixed_string Key> using semantic_field_type_t   = [:m::type_of(field_by_key<T, Key>()):];

// ── rules + violations ────────────────────────────────────────────────────

template <class T> struct rules {
	opt<T>  min{}, max{}, gt{}, lt{}, multiple{};
	opt<sz> max_len{};
	bool    finite{}, nonempty{};
};

enum class violation_kind : u8 { below_min, not_gt, above_max, not_lt, not_multiple, not_finite, empty, too_long, custom };

constexpr sv violation_kind_name(violation_kind k) noexcept {
	constexpr arr<sv, 9> names{"below_min", "not_gt", "above_max", "not_lt", "not_multiple", "not_finite", "empty", "too_long", "custom"};
	auto i = static_cast<sz>(k);
	return i < names.size() ? names[i] : sv{"?"};
}

struct violation    { sz index = npos; ann_text key{}, declared_name{}, message{}; violation_kind kind{}; };
struct check_result { bool ok = true; violation first{}; explicit constexpr operator bool() const noexcept { return ok; } };
struct check_error  : std::runtime_error { fmp::violation violation; explicit check_error(fmp::violation v) : std::runtime_error("fmp validation violation"), violation(v) {} };

namespace d {
template <class T> concept ordered_value  = requires(T const &a, T const &b) { { a < b }    -> std::convertible_to<bool>; };
template <class T> concept sized_value    = requires(T const &v)             { { v.size() } -> std::convertible_to<sz>;   };
template <class T> concept empty_testable = requires(T const &v)             { { v.empty() }-> std::convertible_to<bool>; };

template <class A>         struct check_ann                      { static constexpr bool present = false; };
template <auto Fn, sz Cap> struct check_ann<ann::check<Fn, Cap>> { static constexpr bool present = true; static constexpr auto fn = Fn; };

#define FMP_VAL_ANN(Trait, Ann) \
	template <class A> struct Trait              { static constexpr bool present = false; }; \
	template <auto V>  struct Trait<Ann<V>>      { static constexpr bool present = true; static constexpr auto value = V; }
FMP_VAL_ANN(min_ann,      ann::min);
FMP_VAL_ANN(max_ann,      ann::max);
FMP_VAL_ANN(gt_ann,       ann::gt);
FMP_VAL_ANN(lt_ann,       ann::lt);
FMP_VAL_ANN(multiple_ann, ann::multiple);
#undef FMP_VAL_ANN

template <class A> struct max_len_ann                  { static constexpr bool present = false; };
template <sz N>    struct max_len_ann<ann::max_len<N>> { static constexpr bool present = true; static constexpr sz value = N; };

template <class A>               struct validate_ann                      { static constexpr bool present = false; };
template <fail_action A, bool D> struct validate_ann<ann::validate<A, D>> { static constexpr bool present = true; static constexpr check_policy policy{.action = A, .diagnose_all = D}; };

template <class V, class Bound> consteval V cast_rule_value(Bound v) { return static_cast<V>(v); }

template <class V> consteval void validate_rules(rules<V> c) {
	if ((c.min || c.max || c.gt || c.lt) && !ordered_value<V>) throw "fmp: field bound requires ordered field type";
	if (c.multiple && !std::integral<V>)        throw "fmp: multiple requires integral field type";
	if (c.multiple && *c.multiple == V{})       throw "fmp: multiple cannot be zero";
	if (c.finite   && !std::floating_point<V>)  throw "fmp: finite requires floating-point field type";
	if (c.nonempty && !empty_testable<V>)       throw "fmp: nonempty requires empty()";
	if (c.max_len  && !sized_value<V>)          throw "fmp: max_len requires size()";
	if (c.min && c.gt)                          throw "fmp: duplicate lower bound annotations";
	if (c.max && c.lt)                          throw "fmp: duplicate upper bound annotations";
	if constexpr (ordered_value<V>) {
		if (c.min && c.max && *c.max < *c.min)  throw "fmp: min exceeds max";
		if (c.min && c.lt && !(*c.min < *c.lt)) throw "fmp: min conflicts with lt";
		if (c.gt && c.max && !(*c.gt < *c.max)) throw "fmp: gt conflicts with max";
		if (c.gt && c.lt && !(*c.gt < *c.lt))   throw "fmp: gt conflicts with lt";
	}
}

template <class V> constexpr void clamp_to_rules(V &v, rules<V> c) {
	if constexpr (ordered_value<V>) {
		if (c.min && v < *c.min) v = *c.min;
		if (c.max && *c.max < v) v = *c.max;
		if constexpr (std::integral<V>) {
			if (c.gt && !(*c.gt < v) && *c.gt != std::numeric_limits<V>::max())    v = *c.gt + V{1};
			if (c.lt && !(*c.lt > v) && *c.lt != std::numeric_limits<V>::lowest()) v = *c.lt - V{1};
		} else if constexpr (std::floating_point<V>) {
			if (c.gt && !(*c.gt < v)) v = std::nextafter(*c.gt,  std::numeric_limits<V>::infinity());
			if (c.lt && !(*c.lt > v)) v = std::nextafter(*c.lt, -std::numeric_limits<V>::infinity());
		}
	}
}

template <class Sink, class Token, class V, class C> constexpr void emit_field_violation(Sink &&sink, Token const &tok, V const &v, C const &c, violation const &vio) {
	auto &&s = std::forward<Sink>(sink);
	if      constexpr (requires { std::invoke(s, tok, v, c, vio); })      std::invoke(s, tok, v, c, vio);
	else if constexpr (requires { std::invoke(s, tok, v, c, vio.kind); }) std::invoke(s, tok, v, c, vio.kind);
	else if constexpr (requires { std::invoke(s, tok, v, vio); })         std::invoke(s, tok, v, vio);
	else if constexpr (requires { std::invoke(s, vio); })                 std::invoke(s, vio);
	else if constexpr (requires { std::invoke(s); })                      std::invoke(s);
}

template <class Sink, class T> constexpr void emit_type_violation(Sink &&sink, T const &value, violation const &vio) {
	auto &&s = std::forward<Sink>(sink);
	if      constexpr (requires { std::invoke(s, value, vio); }) std::invoke(s, value, vio);
	else if constexpr (requires { std::invoke(s, vio); })        std::invoke(s, vio);
	else if constexpr (requires { std::invoke(s); })             std::invoke(s);
}

template <class Token, class V, class Sink> constexpr check_result diagnose_value(Token const &tok, V const &v, rules<cvr<V>> c, Sink &&sink) {
	using U = cvr<V>;
	check_result r{};
	auto note = [&](violation_kind kind, ann_text message = {}) {
		violation vio{.index = Token::index, .key = Token::meta.key, .declared_name = Token::meta.declared_name, .message = message, .kind = kind};
		if (r.ok) r = {.ok = false, .first = vio};
		emit_field_violation(sink, tok, v, c, vio);
	};
	if constexpr (std::floating_point<U>) if (c.finite && !std::isfinite(v))    note(violation_kind::not_finite);
	if constexpr (ordered_value<U>) {
		if (c.min && v < *c.min)   note(violation_kind::below_min);
		if (c.gt  && !(*c.gt < v)) note(violation_kind::not_gt);
		if (c.max && *c.max < v)   note(violation_kind::above_max);
		if (c.lt  && !(*c.lt > v)) note(violation_kind::not_lt);
	}
	if constexpr (std::integral<U>)  if (c.multiple && (v % *c.multiple) != 0)  note(violation_kind::not_multiple);
	if constexpr (empty_testable<U>) if (c.nonempty && v.empty())               note(violation_kind::empty);
	if constexpr (sized_value<U>)    if (c.max_len  && v.size() > *c.max_len)   note(violation_kind::too_long);
	return r;
}

template <auto Fn, class Token, class V, class Sink> constexpr check_result diagnose_custom_field(Token const &tok, V const &v, ann_text message, Sink &&sink) {
	bool ok = true;
	if      constexpr (requires { { std::invoke(Fn, v) }      -> std::convertible_to<bool>; }) ok = static_cast<bool>(std::invoke(Fn, v));
	else if constexpr (requires { { std::invoke(Fn, tok, v) } -> std::convertible_to<bool>; }) ok = static_cast<bool>(std::invoke(Fn, tok, v));
	else static_assert(dependent_false_v<V>, "fmp::ann::check field predicate must be bool(V const&) or bool(Token, V const&)");

	if (ok) return {};
	violation vio{.index = Token::index, .key = Token::meta.key, .declared_name = Token::meta.declared_name, .message = message, .kind = violation_kind::custom};
	emit_field_violation(sink, tok, v, rules<cvr<V>>{}, vio);
	return {.ok = false, .first = vio};
}

template <class T, sz I, class Token, class V, class Sink> constexpr check_result diagnose_field_checks(Token const &tok, V const &v, Sink &&sink) {
	check_result out{};
	constexpr mi item = member_info<T, I>();
	constexpr sz n    = annotation_count(item);
	template for (constexpr sz j : index_array<n>()) {
		constexpr mi a = annotation_at(item, j);
		using A = [:ann_t(a):];
		if constexpr (check_ann<A>::present) {
			A anno = m::extract<A>(a);
			auto r = diagnose_custom_field<check_ann<A>::fn>(tok, v, anno.message, sink);
			if (out.ok && !r.ok) out = r;
		}
	}
	return out;
}

template <auto Fn, class T, class Sink> constexpr check_result diagnose_custom_type(T const &value, ann_text message, Sink &&sink) {
	bool ok = true;
	if constexpr (requires { { std::invoke(Fn, value) } -> std::convertible_to<bool>; }) ok = static_cast<bool>(std::invoke(Fn, value));
	else static_assert(dependent_false_v<T>, "fmp::ann::check type predicate must be bool(T const&)");

	if (ok) return {};
	violation vio{.index = npos, .message = message, .kind = violation_kind::custom};
	emit_type_violation(sink, value, vio);
	return {.ok = false, .first = vio};
}

template <class T, class Sink> constexpr check_result diagnose_type_checks(T const &value, Sink &&sink) {
	check_result out{};
	constexpr mi type = m::dealias(^^cvr<T>);
	constexpr sz n    = annotation_count(type);
	template for (constexpr sz j : index_array<n>()) {
		constexpr mi a = annotation_at(type, j);
		using A = [:ann_t(a):];
		if constexpr (check_ann<A>::present) {
			A anno = m::extract<A>(a);
			auto r = diagnose_custom_type<check_ann<A>::fn>(value, anno.message, sink);
			if (out.ok && !r.ok) out = r;
		}
	}
	return out;
}
} // namespace d

template <class T, sz I> consteval auto field_rules() {
	using V = cvr<field_type_t<T, I>>;
	rules<V> out{};
	constexpr mi item = member_info<T, I>();
	constexpr sz n    = d::annotation_count(item);
	template for (constexpr sz j : d::index_array<n>()) {
		constexpr mi a = d::annotation_at(item, j);
		using A = [:d::ann_t(a):];

		#define FMP_TAKE_BOUND(Field, Trait)                                  \
			if constexpr (d::Trait<A>::present) {                             \
				if (out.Field) throw "fmp: duplicate " #Field " annotations"; \
				out.Field = d::cast_rule_value<V>(d::Trait<A>::value);        \
			}
		FMP_TAKE_BOUND(min,      min_ann)
		FMP_TAKE_BOUND(max,      max_ann)
		FMP_TAKE_BOUND(gt,       gt_ann)
		FMP_TAKE_BOUND(lt,       lt_ann)
		FMP_TAKE_BOUND(multiple, multiple_ann)
		#undef FMP_TAKE_BOUND

		if constexpr (std::same_as<A, ann::finite>)   out.finite   = true;
		if constexpr (std::same_as<A, ann::nonempty>) out.nonempty = true;
		if constexpr (d::max_len_ann<A>::present)     { if (out.max_len) throw "fmp: duplicate max_len annotations"; out.max_len = d::max_len_ann<A>::value; }
	}
	d::validate_rules(out);
	return out;
}
template <class T, fixed_string Key> consteval auto field_rules() { return field_rules<T, field_index<T, Key>()>(); }

template <class T, sz I>             inline constexpr auto rules_at_v    = field_rules<T, I>();
template <class T, fixed_string Key> inline constexpr auto field_rules_v = field_rules<T, Key>();

template <class T, class Sink> constexpr check_result diagnose(T const &x, Sink &&sink) {
	check_result out{};
	auto accept = [&](check_result r) { if (out.ok && !r.ok) out = r; };
	each_meta(x, [&]<class Token, class V>(Token tok, V const &v) {
		accept(d::diagnose_value             (tok, v, rules_at_v<cvr<T>, Token::index>, sink));
		accept(d::diagnose_field_checks<cvr<T>, Token::index>(tok, v, sink));
	});
	accept(d::diagnose_type_checks(x, sink));
	return out;
}

template <class T> constexpr check_result validate    (T const &x) { return diagnose(x, [](auto &&...) {}); }
template <class T> constexpr bool         valid       (T const &x) { return static_cast<bool>(validate(x)); }
template <class T> constexpr void         clamp       (T &x)       { each_field(x, []<sz I>(ic<I>, auto &v) { d::clamp_to_rules(v, rules_at_v<cvr<T>, I>); }); }

template <class T> void assert_valid (T const &x) {
#ifndef NDEBUG
	if (!valid(x)) std::abort();
#endif
	(void)x;
}
template <class T> void require_valid(T const &x) { auto r = validate(x); if (!r) throw check_error{r.first}; }

template <class T> check_result enforce(T &x, check_policy p = {}) {
	using enum fail_action;
	switch (p.action) {
	case ignore:       return {};
	case debug_assert: assert_valid(x); return {};
	case reject:       return validate(x);
	case clamp:
		if constexpr (std::is_const_v<std::remove_reference_t<T>>) return validate(x);
		else { fmp::clamp(x); return validate(x); }
	case throw_:       require_valid(x); return {};
	}
	return validate(x);
}

namespace d {
template <class T> consteval opt<check_policy> annotated_check_policy() {
	constexpr mi type = m::dealias(^^cvr<T>);
	if (has_ann<ann::no_validate>(type)) return std::nullopt;

	opt<check_policy> out;
	constexpr sz n = annotation_count(type);
	template for (constexpr sz j : index_array<n>()) {
		constexpr mi a = annotation_at(type, j);
		using A = [:ann_t(a):];
		if constexpr (validate_ann<A>::present) {
			if (out) throw "fmp: duplicate validate annotations";
			out = validate_ann<A>::policy;
		}
	}
	return out;
}
} // namespace d

template <class T> consteval bool         automatic_validation_enabled() { return d::annotated_check_policy<cvr<T>>().has_value(); }
template <class T> inline constexpr bool  automatic_validation_v       = automatic_validation_enabled<T>();

template <class T> consteval check_policy default_check_policy() {
	if (auto p = d::annotated_check_policy<cvr<T>>()) return *p;
	return {.action = fail_action::reject, .diagnose_all = false};
}
template <class T> inline constexpr check_policy default_check_policy_v = default_check_policy<T>();

template <class T> constexpr check_result enforce_automatic(T &x) {
	if constexpr (automatic_validation_v<cvr<T>>) return enforce(x, default_check_policy_v<cvr<T>>);
	else return {};
}
template <class T> constexpr void require_automatic(T &x) {
	if constexpr (automatic_validation_v<cvr<T>>) {
		auto r = enforce(x, default_check_policy_v<cvr<T>>);
		if (!r) throw check_error{r.first};
	}
}

// ── checked<T> ────────────────────────────────────────────────────────────

template <class T> struct patch;
template <class To, class From> constexpr void apply(To &to, patch<From> const &edit);
namespace d { template <class To, class From> constexpr void apply_unchecked(To &to, patch<From> const &edit); }

template <class T, check_policy Policy = default_check_policy_v<T>> class checked {
	static_assert(model<T>, "fmp::checked<T> requires a schema struct");

	T value_;
	struct trusted_tag {};
	constexpr checked(trusted_tag, T value) : value_(std::move(value)) {}

	static constexpr void commit(T &candidate) {
		auto r = enforce(candidate, Policy);
		if (!r) throw check_error{r.first};
	}

      public:
	using value_type = T;
	static constexpr check_policy policy = Policy;

	constexpr          checked() requires std::default_initializable<T> : value_{}             { commit(value_); }
	constexpr explicit checked(T value)                                 : value_(std::move(value)) { commit(value_); }

	static constexpr checked trust(T value) { return checked(trusted_tag{}, std::move(value)); }

	constexpr T const &value     () const & noexcept { return value_; }
	constexpr T const *operator->() const   noexcept { return std::addressof(value_); }
	constexpr T const &operator* () const   noexcept { return value_; }
	constexpr T       *operator->()         noexcept = delete;

	constexpr T        into_inner() const          { return value_; }
	constexpr checked &replace   (T next)          { commit(next); value_ = std::move(next); return *this; }

	template <fixed_string Key, class V> constexpr checked &set   (V &&value)            { T next = value_; get<Key>(next) = std::forward<V>(value); return replace(std::move(next)); }
	template <class P>                   constexpr checked &apply (patch<P> const &edit) { T next = value_; d::apply_unchecked(next, edit);            return replace(std::move(next)); }
	template <class F>                   constexpr checked &mutate(F &&f)                { T next = value_; std::invoke(std::forward<F>(f), next);    return replace(std::move(next)); }
};
template <class T> checked(T) -> checked<cvr<T>>;

template <class T> using auto_model = std::conditional_t<automatic_validation_v<cvr<T>>, checked<cvr<T>>, cvr<T>>;
template <class T> constexpr checked<cvr<T>> check(T &&value) { return checked<cvr<T>>{std::forward<T>(value)}; }

namespace d {
template <class T> consteval mi field_by_runtime_key(sv key) {
	for (auto const &a : fmp::atoms<T>())
		if (a.key.view() == key || a.declared_name == key) return a.member;
	throw "fmp: no such semantic field";
}
} // namespace d

// ── schema fingerprint ────────────────────────────────────────────────────

template <class T, fingerprint_domain Domain = fingerprint_domain::semantic> consteval u64 schema_fingerprint() {
	u64 h = fnv_offset;
	h = hash_sv(h, "fmp.schema.v1");
	for (mi an : m::annotations_of(^^T)) h = hash_sv(h, m::display_string_of(d::ann_t(an)));
	for (auto const &a : atoms<T>()) {
		if      constexpr (Domain == fingerprint_domain::semantic) h = hash_u64(h, a.structural_hash);
		else if constexpr (Domain == fingerprint_domain::layout)   h = hash_u64(h, a.layout_hash);
		else {
			h = hash_u64 (h, a.layout_hash);
			h = hash_byte(h, a.hint_hot      ? 1 : 0);
			h = hash_byte(h, a.hint_cold     ? 1 : 0);
			h = hash_byte(h, a.hint_columnar ? 1 : 0);
			h = hash_byte(h, a.hint_no_wire  ? 1 : 0);
		}
	}
	return h;
}
template <class T> inline constexpr u64 schema_fingerprint_v = schema_fingerprint<T>();

template <class T> consteval u64 semantic_type_hash() {
	u64 h = fnv_offset;
	if      constexpr (std::is_void_v<T>)       h = hash_sv(h, "void");
	else if constexpr (std::same_as<T, bool>)   h = hash_sv(h, "bool");
	else if constexpr (std::integral<T>)        { h = hash_sv(h, std::is_signed_v<T> ? "signed-int" : "unsigned-int"); h = hash_u64(h, sizeof(T)); }
	else if constexpr (std::floating_point<T>)  { h = hash_sv(h, "float"); h = hash_u64(h, sizeof(T)); }
	else if constexpr (std::is_enum_v<T>) {
		h = hash_sv (h, "enum");
		h = hash_u64(h, sizeof(std::underlying_type_t<T>));
		if constexpr (m::is_enumerable_type(^^T))
			for (mi e : m::enumerators_of(^^T)) h = hash_sv(h, d::id(e));
	}
	else if constexpr (model<T>) h = schema_fingerprint<T>();
	else {
		h = hash_sv (h, "opaque");
		h = hash_u64(h, sizeof(T));
		h = hash_u64(h, alignof(T));
	}
	return h;
}

// ── slot / record / kv ────────────────────────────────────────────────────

template <fixed_string Name, class T> struct slot {
	using type = T;
	static constexpr auto name      = Name;
	static constexpr mi   type_info = ^^T;
};

template <class... Slots> struct record_impl {
	struct type;
	consteval { d::def(^^type, mis{d::dm(Slots::type_info, {.name = Slots::name.svv()})...}); }
};
template <class... Slots> using record = typename record_impl<Slots...>::type;

template <class T> struct schema_descriptor { using type = T; };
template <class T> struct is_schema_descriptor                       : std::false_type {};
template <class T> struct is_schema_descriptor<schema_descriptor<T>> : std::true_type  {};
template <class T> inline constexpr bool is_schema_descriptor_v = is_schema_descriptor<cvr<T>>::value;

template <model T> inline constexpr schema_descriptor<T> schema{};

template <fixed_string Name, class T> struct named_value { static constexpr auto name = Name; T value; };
template <fixed_string Name, class T> constexpr named_value<Name, std::decay_t<T>> kv(T &&value) { return {std::forward<T>(value)}; }

template <class R, class... KVs> constexpr R make_record(KVs &&...kvs) {
	R out{};
	((get<cvr<KVs>::name>(out) = std::forward<KVs>(kvs).value), ...);
	require_automatic(out);
	return out;
}

// ── rel (the single correspondence) ───────────────────────────────────────

enum class rel_mode  : u8 { exact, convertible, key_only };
enum class edge_kind : u8 { copy, convert, use_default, ignore_transient, missing, conflict };

constexpr sv edge_kind_name(edge_kind k) noexcept {
	constexpr arr<sv, 6> names{"copy", "convert", "default", "transient", "missing", "conflict"};
	auto i = static_cast<sz>(k);
	return i < names.size() ? names[i] : sv{"?"};
}

struct edge { sz target = npos, source = npos; edge_kind kind = edge_kind::missing; };

namespace d {
template <class Src> consteval sz find_source_key(sv key) {
	auto ss = atoms<Src>();
	for (sz i = 0; i != ss.size(); ++i)
		if (ss[i].key.view() == key) return i;
	return npos;
}

template <class Src, class Dst, sz DI, rel_mode Mode> consteval edge make_edge() {
	auto dt = atoms<Dst>()[DI];
	edge e{.target = DI};

	if (dt.transient || dt.hint_no_wire) { e.kind = edge_kind::ignore_transient; return e; }

	sz si = find_source_key<Src>(dt.key.view());
	if (si == npos) { e.kind = dt.defaulted ? edge_kind::use_default : edge_kind::missing; return e; }

	e.source = si;
	auto st = atoms<Src>()[si];
	if (same_type(st.type, dt.type)) { e.kind = edge_kind::copy; return e; }

	if      constexpr (Mode == rel_mode::convertible) e.kind = edge_kind::convert;
	else                                              e.kind = edge_kind::conflict;
	return e;
}

consteval bool edge_total_without_wire_defaults(field_atom const &target, edge const &e) {
	using enum edge_kind;
	switch (e.kind) {
	case copy: case convert: case ignore_transient: case use_default: return true;
	case missing:  return !target.required;
	case conflict: return false;
	}
	return false;
}
} // namespace d

template <class Src, class Dst, rel_mode Mode = rel_mode::exact> struct rel {
	static_assert(model<Src> && model<Dst>);

	static consteval sz source_count() { return nfields<Src>(); }
	static consteval sz target_count() { return nfields<Dst>(); }

	template <sz DI>       static consteval edge edge()     { static_assert(DI < nfields<Dst>()); return d::make_edge<Src, Dst, DI, Mode>(); }
	template <edge_kind K> static consteval sz   count_of() { sz n = 0; d::for_seq<nfields<Dst>()>([&]<sz I>(ic<I>) { if (edge<I>().kind == K) ++n; }); return n; }

	static consteval bool complete_without_wire_defaults() {
		auto ds = atoms<Dst>(); bool ok = true;
		d::for_seq<nfields<Dst>()>([&]<sz I>(ic<I>) { ok = ok && d::edge_total_without_wire_defaults(ds[I], edge<I>()); });
		return ok;
	}

	static consteval sz   missing_count () { return count_of<edge_kind::missing >(); }
	static consteval sz   conflict_count() { return count_of<edge_kind::conflict>(); }
	static consteval bool equal         () { return nfields<Src>() == nfields<Dst>() && complete_without_wire_defaults() && rel<Dst, Src, Mode>::complete_without_wire_defaults(); }
	static consteval bool contains      () { return complete_without_wire_defaults(); }
};

struct rel_stats { bool complete{}; sz missing{}, conflicts{}; };

template <class Src, class Dst, rel_mode Mode = rel_mode::convertible>
inline constexpr rel_stats rel_stats_v{
    .complete  = rel<Src, Dst, Mode>::complete_without_wire_defaults(),
    .missing   = rel<Src, Dst, Mode>::missing_count(),
    .conflicts = rel<Src, Dst, Mode>::conflict_count(),
};

struct rel_meta {
	sz        target = npos, source = npos;
	edge_kind kind = edge_kind::missing;
	ann_text  target_key{}, source_key{};
	constexpr bool mapped() const noexcept { return source != npos; }
};

namespace d {
template <class Src, class Dst, rel_mode Mode, sz DI> consteval rel_meta rel_meta_at() {
	constexpr auto e = rel<Src, Dst, Mode>::template edge<DI>();
	rel_meta out{.target = e.target, .source = e.source, .kind = e.kind, .target_key = key_at<Dst, DI>()};
	if constexpr (e.source != npos) out.source_key = key_at<Src, e.source>();
	return out;
}
} // namespace d

template <class Src, class Dst, rel_mode Mode = rel_mode::convertible> consteval auto rel_meta_view() {
	return d::with_seq<nfields<Dst>()>([]<sz... Is>(idx_seq<Is...>) { return arr<rel_meta, sizeof...(Is)>{d::rel_meta_at<Src, Dst, Mode, Is>()...}; });
}
template <class Src, class Dst, rel_mode Mode = rel_mode::convertible> inline constexpr auto rel_meta_v = rel_meta_view<Src, Dst, Mode>();

template <class A, class B>       concept schema_equal    = rel<A, B>::equal();
template <class Wide, class Part> concept schema_contains = rel<Wide, Part>::contains();

template <class Wide, class Part> consteval sz schema_missing_count() { return rel<Wide, Part>::missing_count(); }

// ── project / project_with ────────────────────────────────────────────────

namespace d {
template <class To, class From, sz DI> constexpr void assign_one(To &out, From &&from) {
	using Src = cvr<From>;
	constexpr auto e = rel<Src, To, rel_mode::convertible>::template edge<DI>();
	if constexpr      (e.kind == edge_kind::copy)    get<DI>(out) = std::forward<From>(from).[:member_info<Src, e.source>():];
	else if constexpr (e.kind == edge_kind::convert) get<DI>(out) = static_cast<field_type_t<To, DI>>(std::forward<From>(from).[:member_info<Src, e.source>():]);
}

template <class To, class From> constexpr void assign_rel(To &out, From &&from) {
	for_seq<nfields<To>()>([&]<sz I>(ic<I>) { assign_one<To, From, I>(out, std::forward<From>(from)); });
}
} // namespace d

template <class To, class From> requires std::default_initializable<To> constexpr To project(From &&from) {
	static_assert(rel<cvr<From>, To, rel_mode::convertible>::complete_without_wire_defaults(),
	              "fmp::project requires total target coverage; use project_with defaults to totalize missing fields");
	To out{};
	d::assign_rel(out, std::forward<From>(from));
	require_automatic(out);
	return out;
}

// defaults totalize missing fields (incl. required) for project_with / from_wire(..., defaults).
template <class To, class From> constexpr To project_with(From &&from, To defaults) {
	d::assign_rel(defaults, std::forward<From>(from));
	require_automatic(defaults);
	return defaults;
}

// ── pick / struct_union ───────────────────────────────────────────────────

template <class T, fixed_string... Keys> struct pick_impl {
	struct type;
	consteval { static_assert(model<T>); d::def(^^type, mis{d::dm_like(field_by_key<T, Keys>(), {.name = atoms<T>()[field_index<T, Keys>()].key.view()})...}); }
};
template <class T, fixed_string... Keys> using pick = typename pick_impl<T, Keys...>::type;

namespace d {
struct sig { sv key; sv name; mi type; };

consteval sz find_sig(vec<sig> const &xs, sv key) {
	auto it = std::ranges::find(xs, key, &sig::key);
	return it == xs.end() ? npos : static_cast<sz>(it - xs.begin());
}

template <class S> consteval void add_schema_to(mis &out, vec<sig> &sigs) {
	for (auto const &a : atoms<S>()) {
		sz at = find_sig(sigs, a.key.view());
		if (at == npos) {
			sigs.push_back({a.key.view(), a.declared_name, a.type});
			out.push_back(dm_like(a.member, {.name = a.declared_name}));
		} else if (!same_type(sigs[at].type, a.type)) {
			throw "fmp: struct_union conflict; use wide_merge for coproduct conflicts";
		}
	}
}
} // namespace d

template <class... Ss> struct struct_union_impl {
	struct type;
	consteval { static_assert((model<Ss> && ...)); mis out; vec<d::sig> sigs; (d::add_schema_to<Ss>(out, sigs), ...); d::def(^^type, out); }
};
template <class... Ss> using struct_union = typename struct_union_impl<Ss...>::type;

// ── sparse updates (presence + patch) ─────────────────────────────────────

template <class T> struct presence {
      private:
	std::bitset<nfields<T>()> bits_{};
      public:
	constexpr bool test (sz i)                  const          { return bits_.test(i); }
	constexpr void set  (sz i, bool value = true)              { bits_.set(i, value); }
	constexpr void reset(sz i)                                 { bits_.reset(i); }
	constexpr sz   count()                      const noexcept { return bits_.count(); }
	constexpr bool any()                        const noexcept { return bits_.any(); }
	constexpr bool none()                       const noexcept { return bits_.none(); }

	template <fixed_string Key> constexpr bool test ()              const noexcept { return bits_.test(field_index<T, Key>()); }
	template <fixed_string Key> constexpr void set  (bool v = true)       noexcept { bits_.set(field_index<T, Key>(), v); }
	template <fixed_string Key> constexpr void reset()                    noexcept { bits_.reset(field_index<T, Key>()); }
};

template <class T> struct patch {
	T           values{};
	presence<T> present{};

	constexpr sz   count() const noexcept { return present.count(); }
	constexpr bool any  () const noexcept { return present.any();   }
	constexpr bool none () const noexcept { return present.none();  }

	template <fixed_string Key>          constexpr bool   has  () const noexcept { return present.template test<Key>(); }
	template <fixed_string Key, class V> constexpr patch &set  (V &&value)       { get<Key>(values) = std::forward<V>(value); present.template set<Key>(); return *this; }
	template <fixed_string Key>          constexpr patch &reset()       noexcept { present.template reset<Key>(); return *this; }

	template <fixed_string Key, class Default> constexpr auto value_or(Default &&dflt) const {
		using V = cvr<decltype(get<Key>(values))>;
		return has<Key>() ? V(get<Key>(values)) : V(std::forward<Default>(dflt));
	}
};

template <class T, class F> constexpr void each_present(presence<T>     p, F &&f) { d::for_seq<nfields<T>()>([&]<sz I>(ic<I>) { if (p.test(I))         std::invoke(f, field_tag<T, I>{}); }); }
template <class T, class F> constexpr void each_patch  (patch<T> const &p, F &&f) { d::for_seq<nfields<T>()>([&]<sz I>(ic<I>) { if (p.present.test(I)) std::invoke(f, field_tag<T, I>{}, get<I>(p.values)); }); }

template <class T, class... KVs> constexpr patch<T> patch_of(KVs &&...kvs) { patch<T> p{}; ((p.template set<cvr<KVs>::name>(std::forward<KVs>(kvs).value)), ...); return p; }

namespace d {
template <class To, class FromPatch, sz DI> constexpr void apply_one(To &to, FromPatch const &p) {
	constexpr auto e = rel<typename FromPatch::schema_type, To, rel_mode::convertible>::template edge<DI>();
	if constexpr (e.kind == edge_kind::copy || e.kind == edge_kind::convert) {
		if (p.present.test(e.source)) {
			if constexpr (e.kind == edge_kind::copy) get<DI>(to) = get<e.source>(p.values);
			else                                     get<DI>(to) = static_cast<field_type_t<To, DI>>(get<e.source>(p.values));
		}
	}
}
} // namespace d

template <class T> struct typed_patch : patch<T> { using schema_type = T; };
template <class T> constexpr typed_patch<T> typed(patch<T> p) { typed_patch<T> q{}; q.values = std::move(p.values); q.present = p.present; return q; }

namespace d {
template <class To, class From> constexpr void apply_unchecked(To &to, patch<From> const &edit) {
	auto p = typed(edit);
	for_seq<nfields<To>()>([&]<sz I>(ic<I>) { apply_one<To, decltype(p), I>(to, p); });
}
} // namespace d

template <class To, class From> constexpr void apply(To &to, patch<From> const &edit) {
	using U = cvr<To>;
	if constexpr (automatic_validation_v<U>) {
		static_assert(std::copy_constructible<U> && std::assignable_from<To &, U>, "automatic validation needs an atomic copy/commit patch target");
		U next = to;
		d::apply_unchecked(next, edit);
		require_automatic(next);
		to = std::move(next);
	} else {
		d::apply_unchecked(to, edit);
	}
}
template <class To, check_policy Policy, class From> constexpr void apply(checked<To, Policy> &to, patch<From> const &edit) { to.apply(edit); }

// ── delta / diff ──────────────────────────────────────────────────────────

template <class T> struct delta { T before{}; patch<T> edit{}; };

template <class T> constexpr bool changed(delta<T> const &dlt) noexcept { return dlt.edit.present.any(); }

template <class T> constexpr T after(delta<T> const &dlt) { T out = dlt.before; apply(out, dlt.edit); return out; }

template <class T> constexpr delta<T> diff(T const &before, T const &after_value) {
	delta<T> d{.before = before};
	each_field(after_value, [&]<sz I>(ic<I>, auto const &slot) {
		if (!(get<I>(before) == slot)) { get<I>(d.edit.values) = slot; d.edit.present.set(I); }
	});
	return d;
}

// ── wire schemas ──────────────────────────────────────────────────────────

template <class T> struct wire_impl {
	struct type;
	consteval {
		static_assert(model<T>);
		mis xs;
		vec<sv> names;
		for (auto const &a : atoms<T>()) {
			if (a.transient || a.hint_no_wire) continue;
			if (std::ranges::contains(names, a.key.view())) throw "fmp: duplicate wire key";
			names.push_back(a.key.view());
			xs.push_back(d::dm_like(a.member, {.name = a.key.view()}));
		}
		d::def(^^type, xs);
	}
};
template <class T> using wire       = typename wire_impl<T>::type;
template <class T> using wire_patch = patch<wire<T>>;

template <class T>             constexpr wire<T> to_wire  (T const &value)        { return project<wire<T>>(value); }
template <class T, class Wire> constexpr T       from_wire(Wire &&w)              { return project<T>(std::forward<Wire>(w)); }
template <class T, class Wire> constexpr T       from_wire(Wire &&w, T defaults)  { return project_with<T>(std::forward<Wire>(w), std::move(defaults)); }

template <class To, class From> constexpr patch<To> project_patch(patch<From> const &edit) {
	patch<To> out{};
	d::for_seq<nfields<To>()>([&]<sz I>(ic<I>) {
		constexpr auto e = rel<From, To, rel_mode::convertible>::template edge<I>();
		if constexpr (e.kind == edge_kind::copy || e.kind == edge_kind::convert) {
			if (edit.present.test(e.source)) {
				if constexpr (e.kind == edge_kind::copy) get<I>(out.values) = get<e.source>(edit.values);
				else                                     get<I>(out.values) = static_cast<field_type_t<To, I>>(get<e.source>(edit.values));
				out.present.set(I);
			}
		}
	});
	return out;
}

template <class T, class WireT> constexpr patch<T> from_wire_patch(patch<WireT> const &wp) { return project_patch<T>(wp); }

// ── sum (total finite coproduct) ──────────────────────────────────────────

template <class... Ts> class sum {
	static_assert(sizeof...(Ts) > 0);
	static_assert((std::same_as<Ts, cvr<Ts>> && ...), "fmp::sum alternatives must be unqualified value types");

	template <class T> static consteval sz count_of() { return (sz{0} + ... + (std::same_as<cvr<T>, cvr<Ts>> ? sz{1} : sz{0})); }
	static_assert(((count_of<Ts>() == 1) && ...), "fmp::sum requires unique alternatives");

	static constexpr sz n = sizeof...(Ts);
	using index_type = std::conditional_t<n <= 0xFFu,       u8,
	                   std::conditional_t<n <= 0xFFFFu,     u16,
	                   std::conditional_t<n <= 0xFFFFFFFFu, u32, sz>>>;

	static consteval sz max_size () { return std::max({sizeof (Ts)...}); }
	static consteval sz max_align() { return std::max({alignof(Ts)...}); }

	template <sz I> using alt = std::tuple_element_t<I, std::tuple<Ts...>>;

	alignas(max_align()) std::byte storage[max_size()];
	index_type idx = 0;

	template <sz I> constexpr auto *ptr(this auto &&self) noexcept {
		static_assert(I < n);
		using P = std::conditional_t<std::is_const_v<std::remove_reference_t<decltype(self)>>, alt<I> const *, alt<I> *>;
		return std::launder(reinterpret_cast<P>(self.storage));
	}

	constexpr void destroy_active() noexcept((std::is_nothrow_destructible_v<Ts> && ...)) {
		with_index<n>(idx, [&]<sz I>(ic<I>) { std::destroy_at(ptr<I>()); });
	}
	template <sz I, class... As> constexpr void construct_at_alt(As &&...as) {
		std::construct_at(ptr<I>(), std::forward<As>(as)...);
		idx = static_cast<index_type>(I);
	}

	static constexpr bool nothrow_move_v = (std::is_nothrow_move_constructible_v<Ts> && ...);
	static constexpr bool nothrow_swap_v = nothrow_move_v && (std::is_nothrow_swappable_v<Ts> && ...);
	static constexpr bool movable_v      = (std::is_move_constructible_v<Ts> && ...);
	static constexpr bool swappable_v    = ((std::is_move_constructible_v<Ts> && std::is_swappable_v<Ts>) && ...);

      public:
	static constexpr sz size = n;

	constexpr sum() requires std::default_initializable<alt<0>> { construct_at_alt<0>(); }

	template <sz I, class... As> requires (I < n) && std::constructible_from<alt<I>, As...>
	constexpr explicit sum(std::in_place_index_t<I>, As &&...as) { construct_at_alt<I>(std::forward<As>(as)...); }

	template <class T> requires ((std::same_as<cvr<T>, Ts> || ...)) && std::constructible_from<cvr<T>, T>
	constexpr sum(T &&value) { construct_at_alt<index_of<cvr<T>>()>(std::forward<T>(value)); }

	constexpr sum(sum const &rhs) requires ((std::is_copy_constructible_v<Ts> && ...))    { with_index<n>(rhs.idx, [&]<sz I>(ic<I>) { construct_at_alt<I>(           rhs .template get<I>()); }); }
	constexpr sum(sum      &&rhs) noexcept(nothrow_move_v) requires movable_v             { with_index<n>(rhs.idx, [&]<sz I>(ic<I>) { construct_at_alt<I>(std::move(rhs).template get<I>()); }); }

	constexpr ~sum() { destroy_active(); }

	constexpr sum &operator=(sum rhs) noexcept(nothrow_swap_v) requires swappable_v { swap(rhs); return *this; }

	constexpr sz   index() const noexcept { return static_cast<sz>(idx); }

	template <sz I>    constexpr bool holds() const noexcept { return idx == I; }
	template <class T> constexpr bool holds() const noexcept { return index_of<T>() == idx; }

	template <sz I>    constexpr auto &&get(this auto &&self) noexcept { return std::forward_like<decltype(self)>(*self.template ptr<I>()); }
	template <class T> constexpr auto &&as (this auto &&self) noexcept { return std::forward<decltype(self)>(self).template get<index_of<T>()>(); }

	template <class T> static consteval sz index_of() {
		using U = cvr<T>;
		sz out = npos;
		d::for_seq<n>([&]<sz I>(ic<I>) { if constexpr (std::same_as<U, alt<I>>) out = I; });
		if (out == npos) throw "fmp::sum: no such alternative";
		return out;
	}

	template <class F> constexpr decltype(auto) visit(this auto &&self, F &&f) {
		return with_index<n>(self.idx, [&]<sz I>(ic<I>) -> decltype(auto) {
			return std::invoke(std::forward<F>(f), std::forward<decltype(self)>(self).template get<I>());
		});
	}

	template <class Event, class Step> constexpr bool send_if(Event const &event, Step &&step) {
		return with_index<n>(idx, [&]<sz I>(ic<I>) -> bool {
			alt<I> const &state = get<I>();
			if constexpr (requires { sum{std::invoke(std::forward<Step>(step), state, event)}; }) {
				sum next{std::invoke(std::forward<Step>(step), state, event)};
				swap(next);
				return true;
			} else {
				return false;
			}
		});
	}

	template <sz I, class... As>      requires (I < n) && std::constructible_from<alt<I>, As...>                 && nothrow_swap_v
	constexpr alt<I> &emplace(As &&...as) { sum tmp(std::in_place_index<I>, std::forward<As>(as)...); swap(tmp); return get<I>(); }

	template <class T, class... As>   requires ((std::same_as<cvr<T>, Ts> || ...)) && std::constructible_from<cvr<T>, As...> && nothrow_swap_v
	constexpr cvr<T> &emplace(As &&...as) { return emplace<index_of<T>()>(std::forward<As>(as)...); }

	constexpr void swap(sum &other) noexcept(nothrow_swap_v) requires swappable_v {
		if (idx == other.idx) { with_index<n>(idx, [&]<sz I>(ic<I>) { using std::swap; swap(get<I>(), other.template get<I>()); }); return; }
		sum tmp(std::move(*this));
		destroy_active();
		with_index<n>(other.idx, [&]<sz I>(ic<I>) {       construct_at_alt<I>(std::move(other).template get<I>()); });
		other.destroy_active();
		with_index<n>(tmp.idx,   [&]<sz I>(ic<I>) { other.construct_at_alt<I>(std::move(tmp  ).template get<I>()); });
	}

	friend constexpr void swap(sum &a, sum &b) noexcept(noexcept(a.swap(b))) requires swappable_v { a.swap(b); }
};

template <class T>     struct is_sum             : std::false_type {};
template <class... Ts> struct is_sum<sum<Ts...>> : std::true_type  {};
template <class T> inline constexpr bool is_sum_v = is_sum<cvr<T>>::value;

// ── enums as token coproducts ─────────────────────────────────────────────

template <class E> concept reflected_enum = std::is_enum_v<E> && m::is_enumerable_type(^^E);

namespace d {
template <reflected_enum E>                         consteval sz   enum_count()  { return m::enumerators_of(^^E).size(); }
template <reflected_enum E, sz N = enum_count<E>()> consteval auto enum_names()  { arr<sv, N> out{}; auto es = m::enumerators_of(^^E); for (sz i = 0; i != N; ++i) out[i] = id(es[i]);          return out; }
template <reflected_enum E, sz N = enum_count<E>()> consteval auto enum_values() { arr<E,  N> out{}; auto es = m::enumerators_of(^^E); for (sz i = 0; i != N; ++i) out[i] = m::extract<E>(es[i]); return out; }
} // namespace d

template <reflected_enum... Es> struct enum_sum {
	using value_type = sum<Es...>;
	value_type value;

	template <reflected_enum E> static constexpr opt<enum_sum> from(E e) { return enum_sum{.value = value_type(std::in_place_index<value_type::template index_of<E>()>, e)}; }

	static constexpr opt<enum_sum> from_string(sv s) {
		opt<enum_sum> out;
		(([&] {
			constexpr auto names = d::enum_names<Es>();
			constexpr auto vals  = d::enum_values<Es>();
			auto it = std::ranges::find(names, s);
			if (it != names.end()) out = from(vals[static_cast<sz>(it - names.begin())]);
		}()), ...);
		return out;
	}

	static constexpr sv to_string(enum_sum const &x) {
		return x.value.visit([]<class E>(E e) -> sv {
			constexpr auto names = d::enum_names<E>();
			constexpr auto vals  = d::enum_values<E>();
			auto it = std::ranges::find(vals, e);
			return it != vals.end() ? names[static_cast<sz>(it - vals.begin())] : sv{};
		});
	}
};

// ── lens ──────────────────────────────────────────────────────────────────

template <fixed_string Key> struct lens {
	template <class T>                               constexpr decltype(auto) get(T &&x)                       const noexcept { return fmp::get<Key>(std::forward<T>(x)); }
	template <class T, check_policy Policy>          constexpr decltype(auto) get(checked<T, Policy> const &x) const noexcept { return fmp::get<Key>(x.value()); }

	template <class T, class V>                      constexpr void set(T &x,                       V &&value) const { fmp::get<Key>(x) = std::forward<V>(value); }
	template <class T, check_policy Policy, class V> constexpr void set(checked<T, Policy> &x,      V &&value) const { x.template set<Key>(std::forward<V>(value)); }

	template <class T, class F>                      constexpr void over(T &x,                      F &&f)     const { set(x, std::invoke(std::forward<F>(f), fmp::get<Key>(x))); }
	template <class T, check_policy Policy, class F> constexpr void over(checked<T, Policy> &x,     F &&f)     const { x.template set<Key>(std::invoke(std::forward<F>(f), fmp::get<Key>(x.value()))); }
};

// ── flow (derived-field rel) ──────────────────────────────────────────────

template <class A, class B> constexpr auto wide_merge(A &&a, B &&b); // fwd: defined in schema algebra below

namespace d {
template <mi Fn>       consteval auto parameters () { return m::parameters_of(Fn); }
template <mi Fn>       consteval mi   return_type() { return m::return_type_of(Fn); }

template <mi Fn, sz I> consteval sv param_key() { auto ps = parameters<Fn>(); if (I >= ps.size()) throw "fmp: parameter index out of range"; return id(ps[I]); }

template <mi Fn, class State, sz... Is> constexpr decltype(auto) invoke_named(State &&s, idx_seq<Is...>) {
	return std::invoke([:Fn:], (std::forward<State>(s).[:field_by_runtime_key<cvr<State>>(param_key<Fn, Is>()):])...);
}

template <class T> struct is_patch           : std::false_type {};
template <class T> struct is_patch<patch<T>> : std::true_type  {};
template <class T> inline constexpr bool is_patch_v = is_patch<cvr<T>>::value;

template <class State, class Ret> constexpr auto absorb_return(State &&state, Ret &&ret) {
	using R = cvr<Ret>;
	if      constexpr (is_patch_v<R>) { cvr<State> out = std::forward<State>(state); apply(out, std::forward<Ret>(ret)); return out; }
	else if constexpr (model<R>)      { return wide_merge(std::forward<State>(state), std::forward<Ret>(ret)); }
	else static_assert(model<R>, "flow function return must be record-like or patch<T>");
}

template <mi Fn, class State> constexpr auto flow_step(State &&s) {
	auto const &read = s;
	auto ret = invoke_named<Fn>(read, mkidx<parameters<Fn>().size()>{});
	return absorb_return(std::forward<State>(s), std::move(ret));
}

template <class State>                       constexpr decltype(auto) flow_chain(State &&s) { return std::forward<State>(s); }
template <mi First, mi... Rest, class State> constexpr auto           flow_chain(State &&s) {
	auto next = flow_step<First>(std::forward<State>(s));
	if constexpr (sizeof...(Rest) == 0) return next;
	else                                return flow_chain<Rest...>(std::move(next));
}
} // namespace d

template <mi... Steps, class State> constexpr decltype(auto) flow(State &&initial) {
	if constexpr (sizeof...(Steps) == 0) return std::forward<State>(initial);
	else                                 return d::flow_chain<Steps...>(std::forward<State>(initial));
}

template <mi Fn> struct step_descriptor { static constexpr mi fn = Fn; };
template <mi... Fns> struct pipeline_descriptor {};

template <mi Fn> inline constexpr step_descriptor<Fn> step{};
template <mi... Fns> inline constexpr pipeline_descriptor<Fns...> pipeline{};

template <mi A, mi B> constexpr pipeline_descriptor<A, B> operator>>(step_descriptor<A>, step_descriptor<B>) { return {}; }
template <mi... As, mi B> constexpr pipeline_descriptor<As..., B> operator>>(pipeline_descriptor<As...>, step_descriptor<B>) { return {}; }
template <mi A, mi... Bs> constexpr pipeline_descriptor<A, Bs...> operator>>(step_descriptor<A>, pipeline_descriptor<Bs...>) { return {}; }
template <mi... As, mi... Bs> constexpr pipeline_descriptor<As..., Bs...> operator>>(pipeline_descriptor<As...>, pipeline_descriptor<Bs...>) { return {}; }

template <class State, mi Fn>     requires model<cvr<State>> constexpr auto operator>>(State &&state, step_descriptor<Fn>)         { return d::flow_step<Fn>(std::forward<State>(state)); }
template <class State, mi... Fns> requires model<cvr<State>> constexpr auto operator>>(State &&state, pipeline_descriptor<Fns...>) { return flow<Fns...>(std::forward<State>(state)); }

// ── wide merge ────────────────────────────────────────────────────────────

namespace d {
template <class A, class B> consteval mi lub_type_for(field_atom const &key_atom) {
	sz ai = find_source_key<A>(key_atom.key.view());
	sz bi = find_source_key<B>(key_atom.key.view());
	if (ai == npos) return atoms<B>()[bi].type;
	if (bi == npos) return atoms<A>()[ai].type;

	auto ta = atoms<A>()[ai].type;
	auto tb = atoms<B>()[bi].type;
	if (same_type(ta, tb)) return ta;
	return m::substitute(^^sum, {ta, tb});
}

template <class A, class B> consteval vec<field_atom> lub_atoms() {
	vec<field_atom> out;
	auto add = [&](field_atom a) consteval {
		if (std::ranges::contains(out, a.key.view(), [](auto const &old) { return old.key.view(); })) return;
		out.push_back(a);
	};
	for (auto a : atoms<A>()) add(a);
	for (auto b : atoms<B>()) add(b);
	return out;
}
} // namespace d

template <class A, class B> struct wide_merge_impl {
	struct type;
	consteval {
		mis xs;
		for (auto const &a : d::lub_atoms<A, B>()) {
			mi t = d::lub_type_for<A, B>(a);
			xs.push_back(d::same_type(t, a.type) ? d::dm_like(a.member, {.name = a.declared_name})
			                                     : d::dm     (t,        {.name = a.key.view()}));
		}
		d::def(^^type, xs);
	}
};
template <class A, class B> using wide_merge_t = typename wide_merge_impl<cvr<A>, cvr<B>>::type;

template <class A, class B> constexpr auto wide_merge(A &&a, B &&b) {
	using Out = wide_merge_t<A, B>;
	Out out{};

	each_field(out, [&]<sz I>(ic<I>, auto &slot) {
		constexpr auto key = key_at<Out, I>();
		constexpr sz   ai  = d::find_source_key<cvr<A>>(key.view());
		constexpr sz   bi  = d::find_source_key<cvr<B>>(key.view());

		if      constexpr (ai != npos && bi == npos) slot = get<ai>(std::forward<A>(a));
		else if constexpr (ai == npos && bi != npos) slot = get<bi>(std::forward<B>(b));
		else if constexpr (ai != npos && bi != npos) {
			using AT = field_type_t<cvr<A>, ai>;
			using BT = field_type_t<cvr<B>, bi>;
			if constexpr (std::same_as<AT, BT>) slot = get<bi>(std::forward<B>(b));
			else                                slot = cvr<decltype(slot)>(std::in_place_index<1>, get<bi>(std::forward<B>(b)));
		}
	});
	require_automatic(out);
	return out;
}

// ── schema algebra ────────────────────────────────────────────────────────

template <fixed_string... Names> struct key_set { static constexpr sz size = sizeof...(Names); };
template <fixed_string... Names> inline constexpr key_set<Names...> keys{};

namespace d {
consteval sz find_atom(vec<field_atom> const &xs, sv key) {
	for (sz i = 0; i != xs.size(); ++i)
		if (xs[i].key.view() == key) return i;
	return npos;
}

consteval bool compatible_field(field_atom const &a, field_atom const &b) {
	return a.structural_hash == b.structural_hash;
}

template <class A, class B> consteval bool schemas_disjoint() {
	auto bs = atoms<B>();
	for (auto const &a : atoms<A>())
		if (find_atom(bs, a.key.view()) != npos) return false;
	return true;
}

template <class A, class B> consteval bool schemas_joinable() {
	auto bs = atoms<B>();
	for (auto const &a : atoms<A>()) {
		sz bi = find_atom(bs, a.key.view());
		if (bi != npos && !compatible_field(a, bs[bi])) return false;
	}
	return true;
}

consteval mi copied_schema_member(field_atom const &a) {
	return dm_like(a.member, {.name = a.key.view()});
}

template <fixed_string... Keys> consteval bool key_set_contains(sv key) {
	return ((key == Keys.svv()) || ...);
}

template <class Out, class A, class B, sz I> constexpr void assign_join_slot(Out &out, A &&a, B &&b) {
	constexpr auto key = key_at<Out, I>();
	constexpr sz   ai  = find_source_key<cvr<A>>(key.view());
	constexpr sz   bi  = find_source_key<cvr<B>>(key.view());

	if      constexpr (ai != npos && bi == npos) get<I>(out) = get<ai>(std::forward<A>(a));
	else if constexpr (ai == npos && bi != npos) get<I>(out) = get<bi>(std::forward<B>(b));
	else if constexpr (ai != npos && bi != npos) {
		auto &&av = get<ai>(std::forward<A>(a));
		auto &&bv = get<bi>(std::forward<B>(b));
		if constexpr (requires { { av == bv } -> std::convertible_to<bool>; }) { if (!(av == bv)) throw std::logic_error{"fmp::join strict merge overlap mismatch"}; }
		else static_assert(dependent_false_v<Out>, "fmp::join strict merge requires equality-comparable overlapping fields");
		get<I>(out) = get<ai>(std::forward<A>(a));
	}
}

template <class Out, class A, class B> constexpr Out join_rows(A &&a, B &&b) {
	Out out{};
	for_seq<nfields<Out>()>([&]<sz I>(ic<I>) { assign_join_slot<Out, A, B, I>(out, std::forward<A>(a), std::forward<B>(b)); });
	require_automatic(out);
	return out;
}
} // namespace d

template <class A, class B> consteval bool schema_disjoint() { return d::schemas_disjoint<cvr<A>, cvr<B>>(); }
template <class A, class B> consteval bool schema_joinable() { return d::schemas_joinable<cvr<A>, cvr<B>>(); }
template <class A, class B> inline constexpr bool schema_disjoint_v = schema_disjoint<A, B>();
template <class A, class B> inline constexpr bool schema_joinable_v = schema_joinable<A, B>();

template <class A, class B> struct merge_schema_impl {
	struct type;
	consteval {
		static_assert(model<A> && model<B>);
		mis xs; vec<field_atom> seen;
		for (auto const &a : atoms<A>()) { xs.push_back(d::copied_schema_member(a)); seen.push_back(a); }
		for (auto const &b : atoms<B>()) {
			sz at = d::find_atom(seen, b.key.view());
			if      (at == npos)                          { xs.push_back(d::copied_schema_member(b)); seen.push_back(b); }
			else if (!d::compatible_field(seen[at], b))   throw "fmp: schema + requires compatible duplicate fields";
		}
		d::def(^^type, xs);
	}
};
template <class A, class B> using merge_schema_t = typename merge_schema_impl<cvr<A>, cvr<B>>::type;

template <class A, class B> struct product_schema_impl {
	struct type;
	consteval {
		static_assert(model<A> && model<B>);
		mis xs; vec<field_atom> seen;
		for (auto const &a : atoms<A>()) { xs.push_back(d::copied_schema_member(a)); seen.push_back(a); }
		for (auto const &b : atoms<B>()) {
			if (d::find_atom(seen, b.key.view()) != npos) throw "fmp: schema * requires disjoint semantic keys";
			xs.push_back(d::copied_schema_member(b)); seen.push_back(b);
		}
		d::def(^^type, xs);
	}
};
template <class A, class B> using product_schema_t = typename product_schema_impl<cvr<A>, cvr<B>>::type;

template <class A, class B> struct intersect_schema_impl {
	struct type;
	consteval {
		static_assert(model<A> && model<B>);
		mis xs; auto bs = atoms<B>();
		for (auto const &a : atoms<A>()) {
			sz bi = d::find_atom(bs, a.key.view());
			if (bi == npos)                      continue;
			if (!d::compatible_field(a, bs[bi])) throw "fmp: schema & requires compatible shared fields";
			xs.push_back(d::copied_schema_member(a));
		}
		d::def(^^type, xs);
	}
};
template <class A, class B> using intersect_schema_t = typename intersect_schema_impl<cvr<A>, cvr<B>>::type;

template <class A, class B> struct diff_schema_impl {
	struct type;
	consteval { static_assert(model<A> && model<B>); mis xs; auto bs = atoms<B>(); for (auto const &a : atoms<A>()) if (d::find_atom(bs, a.key.view()) == npos) xs.push_back(d::copied_schema_member(a)); d::def(^^type, xs); }
};
template <class A, fixed_string... Keys> struct diff_schema_impl<A, key_set<Keys...>> {
	struct type;
	consteval { static_assert(model<A>); mis xs; for (auto const &a : atoms<A>()) if (!d::key_set_contains<Keys...>(a.key.view())) xs.push_back(d::copied_schema_member(a)); d::def(^^type, xs); }
};
template <class A, class B> using diff_schema_t = typename diff_schema_impl<cvr<A>, cvr<B>>::type;

template <class T, class Keys>                                          struct project_schema_impl;
template <class T, fixed_string... Keys> struct project_schema_impl<T, key_set<Keys...>> { using type = pick<T, Keys...>; };

template <class T, class Keys>           using project_schema_t = typename project_schema_impl<cvr<T>, cvr<Keys>>::type;
template <class T, fixed_string... Keys> using project_keys_t   = project_schema_t<T, key_set<Keys...>>;
template <class T, fixed_string... Keys> using diff_keys_t      = diff_schema_t   <T, key_set<Keys...>>;

template <class T, fixed_string... Keys> using project_t = project_keys_t<T, Keys...>;
template <class T, fixed_string... Keys> using drop_t    = diff_keys_t<T, Keys...>;
template <class A, class B>              using join_t    = merge_schema_t<A, B>;
template <class A, class B>              using product_t = product_schema_t<A, B>;
template <class A, class B>              using meet_t    = intersect_schema_t<A, B>;
template <class A, class B>              using overlay_t = wide_merge_t<cvr<A>, cvr<B>>;

template <class T>                   struct is_key_set                  : std::false_type {};
template <fixed_string... Ks>        struct is_key_set<key_set<Ks...>>  : std::true_type  {};
template <class T> inline constexpr bool is_key_set_v = is_key_set<cvr<T>>::value;

template <class A, class B> constexpr schema_descriptor<merge_schema_t<A, B>>       operator+(schema_descriptor<A>, schema_descriptor<B>) { return {}; }
template <class A, class B> constexpr schema_descriptor<product_schema_t<A, B>>     operator*(schema_descriptor<A>, schema_descriptor<B>) { return {}; }
template <class A, class B> constexpr schema_descriptor<wide_merge_t<cvr<A>, cvr<B>>> operator|(schema_descriptor<A>, schema_descriptor<B>) { return {}; }
template <class A, class B> constexpr schema_descriptor<intersect_schema_t<A, B>>   operator&(schema_descriptor<A>, schema_descriptor<B>) { return {}; }
template <class A, class B> constexpr schema_descriptor<diff_schema_t<A, B>>        operator-(schema_descriptor<A>, schema_descriptor<B>) { return {}; }
template <class A, fixed_string... Keys> constexpr schema_descriptor<diff_schema_t<A, key_set<Keys...>>>    operator-(schema_descriptor<A>, key_set<Keys...>) { return {}; }
template <class A, fixed_string... Keys> constexpr schema_descriptor<project_schema_t<A, key_set<Keys...>>> operator/(schema_descriptor<A>, key_set<Keys...>) { return {}; }

template <class A, class To> requires model<A> && model<To>
constexpr schema_descriptor<To> operator/(schema_descriptor<A>, schema_descriptor<To>) {
	static_assert(rel<A, To, rel_mode::convertible>::contains(), "fmp schema projection requires a total convertible relation");
	return {};
}

template <class T> concept row_like =
	(model<cvr<T>> && !is_schema_descriptor_v<T> && !is_key_set_v<T> && !d::is_patch_v<T>);

template <model To, row_like From> constexpr To project_to(From &&from) {
	return project<To>(std::forward<From>(from));
}

template <row_like A, row_like B> constexpr auto join   (A &&a, B &&b) { return d::join_rows<merge_schema_t    <cvr<A>, cvr<B>>>(std::forward<A>(a), std::forward<B>(b)); }
template <row_like A, row_like B> constexpr auto product(A &&a, B &&b) { return d::join_rows<product_schema_t  <cvr<A>, cvr<B>>>(std::forward<A>(a), std::forward<B>(b)); }
template <row_like A, row_like B> constexpr auto meet   (A &&a, B &&b) { return d::join_rows<intersect_schema_t<cvr<A>, cvr<B>>>(std::forward<A>(a), std::forward<B>(b)); }
template <row_like A, row_like B> constexpr auto overlay(A &&a, B &&b) { return wide_merge(std::forward<A>(a), std::forward<B>(b)); }

template <row_like A, row_like B> constexpr auto operator+(A &&a, B &&b) { return join   (std::forward<A>(a), std::forward<B>(b)); }
template <row_like A, row_like B> constexpr auto operator*(A &&a, B &&b) { return product(std::forward<A>(a), std::forward<B>(b)); }
template <row_like A, row_like B> constexpr auto operator&(A &&a, B &&b) { return meet   (std::forward<A>(a), std::forward<B>(b)); }
template <row_like A, row_like B> constexpr auto operator|(A &&a, B &&b) { return overlay(std::forward<A>(a), std::forward<B>(b)); }

template <row_like A, class To> requires model<To>
constexpr To   operator/(A &&a, schema_descriptor<To>) { return project_to<To>(std::forward<A>(a)); }
template <row_like A, fixed_string... Keys>
constexpr auto operator/(A &&a, key_set<Keys...>)     { return project_to<project_keys_t<cvr<A>, Keys...>>(std::forward<A>(a)); }
template <row_like A, fixed_string... Keys>
constexpr auto operator-(A &&a, key_set<Keys...>)     { return project_to<diff_keys_t   <cvr<A>, Keys...>>(std::forward<A>(a)); }

template <class From, class To> constexpr patch<To> operator/(patch<From> const &edit, schema_descriptor<To>) {
	return project_patch<To>(edit);
}
template <class From, fixed_string... Keys> constexpr auto operator/(patch<From> const &edit, key_set<Keys...>) {
	return project_patch<project_keys_t<From, Keys...>>(edit);
}
template <class From, fixed_string... Keys> constexpr auto operator-(patch<From> const &edit, key_set<Keys...>) {
	return project_patch<diff_keys_t<From, Keys...>>(edit);
}

// ── borrowed table schema views ───────────────────────────────────────────

struct                        owned_lifetime    {};
template <class Owner> struct borrowed_lifetime { Owner const *owner{}; u64 epoch{}; };

// runtime view metadata for borrowed table access; distinct from compile-time schema<T> descriptor.
template <class T, class Lifetime = owned_lifetime> struct schema_view { using schema_type = T; Lifetime lifetime{}; };

template <class T> struct row_ref {
	T const *row{};
	template <fixed_string Key> constexpr decltype(auto) get() const noexcept { return fmp::get<Key>(*row); }
};

template <class T> class table {
	vec<T> rows_;
	u64    epoch_ = 0;
      public:
	using value_type    = T;
	using row_reference = fmp::row_ref<T>;

	constexpr void          reserve(sz n)             { rows_.reserve(n); }
	constexpr bool          empty  ()           const noexcept { return rows_.empty(); }
	constexpr sz            size   ()           const noexcept { return rows_.size(); }
	constexpr row_reference ref    (sz i)       const noexcept { return {std::addressof(rows_[i])}; }
	constexpr T const      &value  (sz i)       const noexcept { return rows_[i]; }
	constexpr T            &value  (sz i)             noexcept requires (!automatic_validation_v<T>) { return rows_[i]; }
	constexpr schema_view<T, borrowed_lifetime<table>> view() const noexcept { return {.lifetime = {this, epoch_}}; }

	constexpr sz push(T v) { require_automatic(v); rows_.push_back(std::move(v)); ++epoch_; return rows_.size() - 1; }
	template <class... KVs> constexpr sz push_values(KVs &&...kvs) { T row{}; ((fmp::get<cvr<KVs>::name>(row) = std::forward<KVs>(kvs).value), ...); return push(std::move(row)); }

	template <class F> constexpr void mutate(sz i, F &&f) {
		if constexpr (automatic_validation_v<T>) { T next = rows_[i]; std::invoke(std::forward<F>(f), next); require_automatic(next); rows_[i] = std::move(next); }
		else                                       std::invoke(std::forward<F>(f), rows_[i]);
		++epoch_;
	}
	template <fixed_string Key, class V> constexpr void set(sz i, V &&value) { mutate(i, [&](T &row) { fmp::get<Key>(row) = std::forward<V>(value); }); }

	template <model R = T>      constexpr R              get         (sz i)              const { if constexpr (std::same_as<R, T>) return rows_[i]; else return project<R>(rows_[i]); }
	template <model R>          constexpr decltype(auto) project_view_(T const &row)     const { if constexpr (std::same_as<R, T>) return (row);    else return project<R>(row); }

	template <fixed_string Key> constexpr auto column() const {
		vec<cvr<semantic_field_type_t<T, Key>>> out;
		out.reserve(rows_.size());
		for (auto const &row : rows_) out.push_back(fmp::get<Key>(row));
		return out;
	}

	template <model R = T, class F>    constexpr void    for_each_row_value(F &&f)    const { for (sz i = 0; i != rows_.size(); ++i) std::invoke(f, i, project_view_<R>(rows_[i])); }
	template <model R = T, class Pred> constexpr opt<sz> find_if           (Pred &&p) const { for (sz i = 0; i != rows_.size(); ++i) if (std::invoke(p, project_view_<R>(rows_[i]))) return i; return std::nullopt; }
	template <model R = T, class Pred> constexpr vec<sz> select            (Pred &&p) const { vec<sz> out; for (sz i = 0; i != rows_.size(); ++i) if (std::invoke(p, project_view_<R>(rows_[i]))) out.push_back(i); return out; }

	template <class P> constexpr void patch(sz row, fmp::patch<P> const &edit) { apply(rows_[row], edit); ++epoch_; }

	template <model R = T, class P, class Pred> constexpr sz patch_if(fmp::patch<P> const &edit, Pred &&p) {
		sz changed = 0;
		for (sz i = 0; i != rows_.size(); ++i) if (std::invoke(p, project_view_<R>(rows_[i]))) { patch(i, edit); ++changed; }
		return changed;
	}

	template <class Pred> constexpr vec<row_reference> where_ref(Pred &&p) const {
		vec<row_reference> out;
		for (sz i = 0; i != rows_.size(); ++i) { auto rr = ref(i); if (std::invoke(p, rr)) out.push_back(rr); }
		return out;
	}
};

} // namespace fmp
