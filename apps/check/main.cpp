// ron-check: parse and pose every unit asset referenced by the game's graphics catalog.
// Usage: ron-check <game-data-dir>
// Exit code 0 when every present file parses; missing files are reported but tolerated
// (the shipped catalog references a few animations that do not exist).
#include <algorithm>
#include <filesystem>
#include <array>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <map>
#include <memory>
#include <set>
#include <string>

#include "assets/bh3.hpp"
#include "assets/bha.hpp"
#include "assets/catalog.hpp"
#include "assets/effects.hpp"
#include "assets/battlefield.hpp"
#include "assets/buildings.hpp"
#include "assets/rules.hpp"
#include "assets/scenario.hpp"
#include "assets/tileset.hpp"
#include "assets/pose.hpp"
#include "assets/tga.hpp"
#include "sim/rules.hpp"
#include "sim/world_load.hpp"

int main(int argc, char** argv) {
	if (argc < 2) {
		std::fprintf(stderr, "usage: %s <game-data-dir>\n", argv[0]);
		return 2;
	}
	const auto t0 = std::chrono::steady_clock::now();
	try {
		const ron::GameFiles files(argv[1]);
		const auto units = ron::load_unit_catalog(files);
		std::printf("catalog: %zu unit graphics\n", units.size());

		std::set<std::string> models, textures, anims, missing, data_errors;
		for (const auto& u : units) {
			models.insert(u.model);
			if (!u.texture.empty()) textures.insert(u.texture);
			for (const auto& a : u.anims) {
				if (!a.file.empty()) anims.insert(a.file);
			}
		}

		int failures = 0;
		auto report = [&](const std::string& kind, const std::string& f, const char* what) {
			std::printf("FAIL %s %s: %s\n", kind.c_str(), f.c_str(), what);
			failures++;
		};

		std::map<std::string, ron::BH3Model> parsed_models;
		for (const auto& m : models) {
			const std::string path = files.resolve(m);
			if (path.empty()) { missing.insert(m); continue; }
			try { parsed_models.emplace(m, ron::parse_bh3(ron::read_file(path))); }
			catch (const std::exception& e) { report("model", m, e.what()); }
		}
		std::map<std::string, ron::Animation> parsed_anims;
		for (const auto& a : anims) {
			const std::string path = files.resolve(a);
			if (path.empty()) { missing.insert(a); continue; }
			try { parsed_anims.emplace(a, ron::parse_bha(ron::read_file(path))); }
			catch (const std::exception& e) { report("anim", a, e.what()); }
		}
		size_t texels = 0;
		for (const auto& t : textures) {
			// The shipped catalog has texture= entries that point at non-textures (HERDSEALS-TYPE2 uses
			// sealbrown.bh3). That is a bug in the game's data, not ours, so list it apart from failures.
			std::string ext = t.size() >= 4 ? t.substr(t.size() - 4) : t;
			for (char& c : ext) c = char(std::tolower(static_cast<unsigned char>(c)));
			if (ext != ".tga") { data_errors.insert("texture " + t); continue; }
			const std::string path = files.resolve(t);
			if (path.empty()) { missing.insert(t); continue; }
			try {
				const auto img = ron::parse_tga(ron::read_file(path));
				texels += size_t(img.width) * img.height;
			} catch (const std::exception& e) { report("texture", t, e.what()); }
		}

		// Pose every unit with each of its animations at a few times; check for NaNs.
		size_t poses = 0;
		std::vector<ron::PosedVertex> verts;
		for (const auto& u : units) {
			auto mi = parsed_models.find(u.model);
			if (mi == parsed_models.end()) continue;
			for (const auto& a : u.anims) {
				auto ai = parsed_anims.find(a.file);
				if (ai == parsed_anims.end()) continue;
				for (uint32_t ms : {0u, 250u, 1000u}) {
					ron::pose_model(mi->second, &ai->second, ms, verts);
					poses++;
					for (const auto& v : verts) {
						if (!std::isfinite(v.px) || !std::isfinite(v.py) || !std::isfinite(v.pz)) {
							report("pose", u.name + " / " + a.action, "non-finite vertex");
							break;
						}
					}
				}
			}
		}

		// Attachments (EXISTEVENT): every type must name an AMMO piece; count events whose slot the model lacks
		// (the game skips those too; docs/attachments-spec.md expects 59).
		const auto ammo = ron::load_ammo_catalog(files, ron::load_anim_names(files));
		size_t events = 0, unresolved_type = 0, empty_slot = 0;
		const bool verbose = std::getenv("RON_CHECK_VERBOSE") != nullptr;
		std::set<std::string> attach_models;
		for (const auto& u : units) {
			auto mi = parsed_models.find(u.model);
			const auto slots = mi == parsed_models.end() ? std::array<int, ron::kAttachSlots>{} : ron::attach_slots(mi->second);
			for (const auto& ev : u.exist_events) {
				events++;
				const ron::AmmoPiece* piece = ammo.find(ev.type);
				if (!piece) {
					unresolved_type++;
					data_errors.insert("existevent type " + ev.type);
					continue;
				}
				attach_models.insert(piece->model);
				if (mi != parsed_models.end() && ev.node >= 0 && (ev.node >= ron::kAttachSlots || slots[size_t(ev.node)] < 0)) {
					empty_slot++;
					if (verbose) std::printf("  empty slot: %s (%s) %s node %d\n", u.name.c_str(), u.model.c_str(), ev.type.c_str(), ev.node);
				}
			}
		}
		size_t attach_ok = 0;
		for (const auto& m : attach_models) {
			const std::string path = files.resolve(m);
			if (path.empty()) { missing.insert(m); continue; }
			try { ron::parse_bh3(ron::read_file(path)); attach_ok++; }
			catch (const std::exception& e) { report("attachment model", m, e.what()); }
		}

		// Scenarios: decode every shipped .scx/.sce terrain and check its tileset's textures resolve.
		size_t scenarios = 0, placed = 0, unresolved_objects = 0;
		std::set<std::string> tilesets_seen;
		std::vector<std::pair<std::string, std::string>> scx_paths;  // (relative, absolute), for the world grid below
		const ron::Rules rules = ron::load_rules(files);
		const ron::BuildCatalog build_catalog = ron::load_build_catalog(files, ron::load_anim_names(files));
		{
			std::error_code ec;
			std::vector<std::string> paths;
			for (auto it = std::filesystem::recursive_directory_iterator(files.root(), ec);
			     !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
				const std::string ext = ron::to_lower(it->path().extension().string());
				if (it->is_regular_file(ec) && (ext == ".scx" || ext == ".sce")) paths.push_back(it->path().string());
			}
			std::sort(paths.begin(), paths.end());
			for (const std::string& path : paths) {
				const std::string rel = std::filesystem::relative(path, files.root(), ec).generic_string();
				try {
					const ron::Scenario sc = ron::parse_scenario(ron::read_file(path));
					scenarios++;
					if (ron::to_lower(rel).size() > 4 && ron::to_lower(rel).compare(rel.size() - 4, 4, ".scx") == 0) {
						scx_paths.emplace_back(rel, path);
						const ron::Battlefield bf = ron::resolve_battlefield(files, sc, rel, rules, build_catalog, units);
						placed += bf.objects.size();
						unresolved_objects += bf.unresolved.size();
						if (verbose) {
							for (const std::string& u : bf.unresolved) std::printf("  unresolved object in %s: %s\n", rel.c_str(), u.c_str());
						}
					}
					if (tilesets_seen.insert(ron::to_lower(sc.terrain.tileset)).second) {
						const ron::Tileset ts = ron::load_tileset(files, sc.terrain.tileset);
						for (const auto* list : {&ts.baseland, &ts.coastal, &ts.ocean}) {
							for (const auto& tex : *list) {
								if (tex.file.empty()) missing.insert("tileset " + ts.name + " texture " + tex.name);
							}
						}
						if (ts.baseland.empty()) report("tileset", ts.name, "no BASELAND textures");
					}
				} catch (const std::exception& e) { report("scenario", rel, e.what()); }
			}
		}

		// Simulation rules tables (engine/sim): load, finalise for a default game and serialise the rules image
		// (1,024,221 bytes for the stock data; ron-rules compares it with a replay).
		std::string rules_summary = "not loaded";
		ron::sim::Rules sim_rules;
		bool sim_rules_ok = false;
		try {
			sim_rules = ron::sim::load_rules(files);
			ron::sim::start_game(sim_rules, ron::sim::GameSetup());
			const size_t image_bytes = ron::sim::write_image(sim_rules).size();
			size_t kinds[7] = {};
			for (const ron::sim::Type& t : sim_rules.types) kinds[int(t.kind)]++;
			char buf[256];
			std::snprintf(buf, sizeof buf, "%zu goods, %zu units, %zu buildings, %zu techs, %zu crafts, %zu bonuses, %zu tribes; "
			              "image %zu bytes, %zu warnings", kinds[0], kinds[1], kinds[2], kinds[4], kinds[5], kinds[6],
			              sim_rules.tribes.size(), image_bytes, sim_rules.warnings.size());
			rules_summary = buf;
			sim_rules_ok = true;
		} catch (const std::exception& e) { report("rules", "Data/*.xml", e.what()); }

		// Simulation world grid (engine/sim/world_load): build every .scx's grid with its objects; the per-wcoord
		// counters must agree with the TData bits, and the CollBlock collector must keep exactly the occupied blocks.
		std::string world_summary = "not built (no rules)";
		if (sim_rules_ok) {
			size_t grids = 0, stamps = 0, regions = 0, buildings = 0, unit_cells = 0;
			for (const auto& [rel, path] : scx_paths) {
				try {
					const ron::Scenario sc = ron::parse_scenario(ron::read_file(path));
					auto wb = std::make_unique<ron::sim::WorldBuild>();
					ron::sim::build_world(*wb, files, sc, &sim_rules, 0);
					ron::sim::World& w = wb->world;
					long sum_blocked = 0, sum_bad = 0, n_blocked = 0, n_bad = 0;
					for (const ron::sim::WData& d : w.wdata) sum_blocked += d.blocked, sum_bad += d.bad;
					for (uint16_t m : w.tdata) n_blocked += (m & ron::sim::tmask::kBlocked) != 0, n_bad += (m & ron::sim::tmask::kBadPath) != 0;
					if (sum_blocked != n_blocked || sum_bad != n_bad) report("world", rel, "WData counters disagree with TData bits");
					size_t live = 0;
					for (const ron::sim::WData& d : w.wdata) live += d.block >= 0;
					for (int32_t f = 0; f <= w.size / std::max(w.xs / 4, 5) + 1; f++) w.process_coll_blocks();
					size_t kept = 0;
					for (const ron::sim::WData& d : w.wdata) kept += d.block >= 0;
					if (kept != live) report("world", rel, "process_coll_blocks freed an occupied block");
					grids++;
					stamps += wb->mountains.size();
					for (const ron::sim::Region& g : wb->regions.r) regions += g.size != 0;
					buildings += size_t(wb->objects.builds + wb->objects.cities);
					unit_cells += size_t(wb->objects.unit_cells);
				} catch (const std::exception& e) { report("world", rel, e.what()); }
			}
			char buf[256];
			std::snprintf(buf, sizeof buf, "%zu/%zu .scx grids built; %zu mountain stamps, %zu regions, %zu buildings, "
			              "%zu unit ucoords", grids, scx_paths.size(), stamps, regions, buildings, unit_cells);
			world_summary = buf;
		}

		const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
		std::printf("models: %zu ok / %zu referenced\n", parsed_models.size(), models.size());
		std::printf("anims:  %zu ok / %zu referenced\n", parsed_anims.size(), anims.size());
		std::printf("textures: %zu referenced, %.1f Mtexels decoded\n", textures.size(), texels / 1e6);
		std::printf("poses evaluated: %zu\n", poses);
		std::printf("attachments: %zu events, %zu unresolved types, %zu on a missing slot; %zu/%zu models ok\n", events,
		            unresolved_type, empty_slot, attach_ok, attach_models.size());
		std::printf("scenarios: %zu decoded, %zu tilesets; %zu objects placed, %zu without a graphic (.scx)\n", scenarios,
		            tilesets_seen.size(), placed, unresolved_objects);
		std::printf("rules: %s\n", rules_summary.c_str());
		std::printf("world: %s\n", world_summary.c_str());
		std::printf("missing files: %zu", missing.size());
		for (const auto& m : missing) std::printf("\n  %s", m.c_str());
		std::printf("\ngame data errors: %zu", data_errors.size());
		for (const auto& d : data_errors) std::printf("\n  %s", d.c_str());
		std::printf("\nfailures: %d (%.2fs)\n", failures, secs);
		return failures ? 1 : 0;
	} catch (const std::exception& e) {
		std::fprintf(stderr, "error: %s\n", e.what());
		return 1;
	}
}
