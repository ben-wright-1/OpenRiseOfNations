// Access to a user's game install: case-insensitive lookup of the game's backslash-separated paths.
#pragma once

#include <map>
#include <string>
#include <vector>

namespace ron {

// Resolves the game's case-insensitive, backslash-separated paths against a directory tree.
class GameFiles {
public:
	explicit GameFiles(std::string root);
	// Returns the absolute path of `relative` (any case, '\' or '/', optional leading ".\"), or "" if absent.
	std::string resolve(const std::string& relative) const;
	// Reads a text file by its game-relative path; throws if it is missing.
	std::string read_text(const std::string& relative) const;
	const std::string& root() const { return root_; }
	// Lowercase relative paths of the files directly inside `dir` (case-insensitive, '/'-separated), sorted.
	std::vector<std::string> list_dir(const std::string& dir) const;

private:
	std::string root_;
	std::map<std::string, std::string> by_lower_;  // lowercase relative path -> actual absolute path
};

// Normalize a path as written in the game's XML: strip ".\" and convert '\' to '/'.
std::string normalize_game_path(const std::string& p);

std::string to_lower(std::string s);

}  // namespace ron
