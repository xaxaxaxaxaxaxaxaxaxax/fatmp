//   import std / import fmp   C++ modules
//   [[= ... ]]                experimental reflection annotations
//   wire/pick/record/slot     fmp schema builders
//   rel/table/patch/kv        fmp mapping, storage, and sparse update helpers
//   schema/keys             schema algebra descriptors and row operations

import std;
import fmp;

namespace demo {
using namespace fmp;
using namespace fmp::ann;
using namespace fmp::hint;

using id_t = std::uint64_t;

// annotations take the form
//   [[= annotation{}, = annotation{value}, ... ]] member;
// currently, the exposed annotation set is limited, but will be extended.
//
// annotations essentially act as a kv store at compiletime; and let you
// treat arbitrary structs as typed kv pairs.
//
// key{"wire_name"} is only needed when the semantic or wire key differs from
// the C++ member name. the other annotations below add behavior used later in
// the example: id marks the identity field, min/max/nonempty/max_len drive
// validation, and defaulted permits a missing source field to use the member
// initializer (i.e {})
//
// transient and no_wire are both local-only hints but are slightly distinct.
// transient says relation/project is allowed to ignore this target field;
// this means that runtime models may not possess this field after mutation.
// no_wire says generated wire<T> should not contain this field, most likely
// because we want to use it once only to determine a mutation quality.
//
// wire<T> also drops transient fields.
// the type-level validate annotation defines checked-access behaviour.
// table<T> uses it on push/mutate/set; with fail_action::reject, a bad write throws
// before the stored value changes.
//
// validation is also available directly through valid(value), validate(value),
// and diagnose(value, sink).
struct  [[= ann::validate<fail_action::reject, true>{} ]] Model {
	[[= key{"id"},    = id{},        = min<1ull>{}   ]] id_t        model_id{};
	[[= key{"name"},  = nonempty{},  = max_len<16>{} ]] std::string name{};
	[[= key{"count"}, = min<0>{},    = max<100>{}    ]] int         count{};
	[[= key{"score"}, = min<0l>{},   = max<1000l>{}  ]] long        score{};
	[[= key{"tag"},   = defaulted{}, = max_len<8>{}  ]] std::string tag{"default"};
	[[= transient{}                                  ]] bool        dirty{};
	[[= no_wire{}                                    ]] id_t        local_version{};
};

// this schema deliberately uses the same external keys but different C++ member names.
// as we will see below, projection matches on key, so source_id can still become model_id
// in spite of the different C++ name.
struct Input {
	[[= key{"id"},    = id{} ]] id_t        source_id{};
	[[= key{"name"}          ]] std::string label{};
	[[= key{"count"}         ]] short       n{};
	[[= key{"score"}         ]] int         points{};
};

// fmp defines the following schema/model type builders:
//   wire<T>                 T with transient/no_wire fields removed
//   pick<T, "key"...>       a keyed subset of T
//   record<slot<...>...>    an ad-hoc schema without another struct declaration
//   schema<T>               a zero-value descriptor used to compose schema types
//
// wire<Model> has id/name/count/score/tag and leaves out dirty
// and local_version ( because they are marked as local-hints, remember?)
//
// A schema is the reflected shape: keys, fields, annotations, and relations.
// record<slot<...>> is a concrete generated aggregate with storage. It exists
// for small typed rows that still need schema metadata:
// evidence/logging/output rows, little join results, temporary projections, etc.
// the point is to avoid naming a throwaway aggregate while keeping key lookup,
// type checking, and each_meta/print/debug support, etc.

using Wire = wire<Model>;

// schema<T> is the type-side handle for the same algebra that row values use.
// operators are nice when you already have values, or when you are writing a
// one-off schema expression, see below:
//   a / keys<...>    projection by semantic key
//   a / schema<T>    projection to a named schema
//   a - keys<...>    drop fields by semantic key
//   a + b            strict compatible merge; shared fields must match
//   a * b            disjoint product; shared keys are rejected
//   a & b            compatible intersection
//   a | b            overlay / wide merge; type conflicts become sum<...>
//
// for named result types, prefer the aliases below
// : project_t, drop_t, join_t, product_t, meet_t, overlay_t.

using View = project_t<Model, "id", "name", "count", "tag">;
using RowInfo = record<
    slot<"kind", std::string>,
    slot<"id",   id_t>,
    slot<"name", std::string>,
    slot<"ok",   bool>>;
using Extra = record<
    slot<"kind", std::string>,
    slot<"ok",   bool>>;

using ModelIdentity = project_t<Model, "id", "name">;
using ModelStored = drop_t<Model, "dirty", "local_version">;
using ModelViewOverlap = meet_t<Model, View>;
using ModelPlusView = join_t<Model, View>;
using InputProduct = product_t<Input, Extra>;
using InputOrView = overlay_t<Input, View>;

// using ModelIdentity = schema<Model> / keys<"id", "name">;
// using ModelStored   = schema<Model> - keys<"dirty", "local_version">;
// using ModelOverlap  = schema<Model> & schema<View>;
// using ModelJoined   = schema<Model> + schema<View>;
// using InputProduct  = schema<Input> * schema<Extra>;
// using InputOverlay  = schema<Input> | schema<View>;

// notice that input can map to model when considering convertible schemas,
// but NOT exact mappings, because count and score have different source types.
// we can determine subset admission via schema_contains, which matches on key.
// rel is comptime proof/plan, i.e allows strong conversions to be validated
// and "safe" before execution.
static_assert(rel<Input, Model, rel_mode::convertible>::contains());
static_assert(!rel<Input, Model, rel_mode::exact>::contains());
static_assert(schema_contains<Model, View>);
static_assert(nfields<Wire>() == 5);
static_assert(nfields<ModelIdentity>() == 2);
static_assert(nfields<ModelStored>() == 5);
static_assert(nfields<ModelViewOverlap>() == 4);
static_assert(nfields<ModelPlusView>() == nfields<Model>());
static_assert(nfields<InputProduct>() == nfields<Input>() + nfields<Extra>());
static_assert(nfields<InputOrView>() == 5);
static_assert(has_field<InputProduct, "kind">());
static_assert(!has_field<ModelStored, "local_version">());
static_assert(std::same_as<semantic_field_type_t<InputOrView, "count">, sum<short, int>>);

// print the reflected shape of any schema-like type
// fmp provides the schema_fingerprint hash option, as well
// as a fields_v to iterate for all generated models.
template <model T> void print_schema(std::string_view label) {
	std::cout << label << " fields=" << nfields<T>()
	          << " fingerprint=" << schema_fingerprint_v<T> << '\n';

	for (const auto &f : fields_v<T>) {
		std::cout << "  " << f.name();
		if (f.key_view() != f.name()) std::cout << " key=" << f.key_view();
		if (f.id)                    std::cout << " id";
		if (f.defaulted)             std::cout << " defaulted";
		if (f.transient)             std::cout << " transient";
		if (f.hint_no_wire)          std::cout << " no_wire";
		std::cout << '\n';
	}
}

// print the field-by-field plan fmp will use for a projection or wire lift.
// the interesting bit is the edge kind: copy, convert, default, transient, etc.
template <model Src, model Dst> void print_rel(std::string_view label) {
	constexpr auto stats = rel_stats_v<Src, Dst, rel_mode::convertible>;
	std::cout << label << " complete=" << stats.complete
	          << " missing=" << stats.missing
	          << " conflicts=" << stats.conflicts << '\n';

	for (const auto &e : rel_meta_v<Src, Dst, rel_mode::convertible>) {
		std::cout << "  " << e.target_key.view() << " <- ";
		if (e.mapped()) std::cout << e.source_key.view();
		else            std::cout << "-";
		std::cout << " " << edge_kind_name(e.kind) << '\n';
	}
}

// print values by reflected key order instead of hard-coding members.
template <model T> void print_value(std::string_view label, const T &value) {
	std::cout << label;
	each_meta(value, [](auto field, const auto &v) {
		std::cout << ' ' << field.key() << '=';
		if constexpr (is_sum_v<decltype(v)>) v.visit([](const auto &alt) { std::cout << alt; });
		else                                std::cout << v;
	});
	std::cout << '\n';
}

// patches are sparse, so only present fields are printed.
template <model T> void print_patch(std::string_view label, const patch<T> &p) {
	std::cout << label << " fields=" << p.count();
	each_patch(p, [](auto field, const auto &v) {
		std::cout << ' ' << field.key() << '=' << v;
	});
	std::cout << '\n';
}

// diagnose reports every failing rule, not just the first one.
void print_errors(const Model &value) {
	(void)diagnose(value, [](const violation &v) {
		std::cout << "  " << v.key.view() << " " << violation_kind_name(v.kind) << '\n';
	});
}

} // namespace demo

int main() {
	using namespace demo;
	std::cout << std::boolalpha;

	// generated and ad-hoc schema shapes still expose the same metadata.
	print_schema<Model>("Model");
	print_schema<Wire>("Wire");
	print_schema<View>("View");
	print_schema<RowInfo>("RowInfo");
	print_schema<ModelIdentity>("ModelIdentity");
	print_schema<ModelStored>("ModelStored");
	print_schema<InputOrView>("InputOrView");

	// arbitrary rels give the copy/convert/default plan before values move.
	// these are comptime generated, and are pretty useful
	print_rel<Input, Model>("Input -> Model");
	print_rel<Model, Wire>("Model -> Wire");


	// projections are runtime conversions using the rel metadata.
	// Input has source_id/label/n/points, but input / schema<Model> writes
	// model_id/name/count/score because the keys match.
	Input input{.source_id = 7, .label = "alpha", .n = 3, .points = 42};
	Model model = input / schema<Model>;
	model.dirty = true;
	model.local_version = 1;

	// wire and pick are just other schema-shaped values built from Model.
	// make_record is the record<slot<...>> constructor: kv binds a key to a value.
	Wire wire = model / schema<Wire>;
	View view = model / schema<View>;
	RowInfo info = make_record<RowInfo>(
	    kv<"kind">(std::string{"example"}),
	    kv<"id">(model.model_id),
	    kv<"name">(model.name),
	    kv<"ok">(valid(model)));
	Extra extra = make_record<Extra>(
	    kv<"kind">(std::string{"derived"}),
	    kv<"ok">(true));

	print_value("input", input);
	print_value("model", model);
	print_value("wire", wire);
	print_value("view", view);
	print_value("record", info);

	auto identity = model / keys<"id", "name">;
	auto stored_shape = model - keys<"dirty", "local_version">;
	auto product = input * extra;
	auto strict = model + view;
	auto wide = input | view;
	print_value("algebra identity", identity);
	print_value("algebra stored", stored_shape);
	print_value("algebra product", product);
	print_value("algebra strict", strict);
	print_value("algebra wide", wide);

	// table<T> exists for the other side of the same idea: keep real typed rows,
	// but expose keyed row/field operations. you get row storage, projection,
	// sparse patch application, columns, and validation in one place, without
	// turning everything into maps of strings to variants.
	table<Model> rows;
	const sz row = rows.push(model);
	rows.set<"count">(row, 4);

	// patches are typed sparse updates. this one starts as a patch<Input>;
	// Patch projection lifts it through the same key relation as row projection.
	auto input_patch = patch_of<Input>(
	    kv<"name">("beta"),
	    kv<"score">(77));
	auto model_patch = input_patch / schema<Model>;
	print_patch("lifted patch", model_patch);
	rows.patch(row, model_patch);

	// diff compares two full values and returns a sparse patch containing only
	// changed fields. here: score and tag.
	Model before = rows.get(row);
	Model after = before;
	after.tag = "patched";
	after.score = 99;
	auto change = diff(before, after);
	print_patch("diff", change.edit);
	rows.patch(row, change.edit);

	print_value("stored", rows.get(row));

	// this write violates max<100>, so the table rejects it before changing
	// the stored row.
	try {
		rows.set<"count">(row, 101);
	} catch (const check_error &e) {
		std::cout << "rejected " << e.violation.key.view()
		          << " " << violation_kind_name(e.violation.kind) << '\n';
	}

	std::cout << "bad model diagnostics\n";
	print_errors(Model{.model_id = 0, .name = "", .count = -1, .score = 2000, .tag = "too-long-tag"});
}
