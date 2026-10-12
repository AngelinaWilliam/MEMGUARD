// memguard_instrument - writes an instrumented copy of Pawn sources for MemGuard X
// function profiling. The original files are never modified.
//
//   memguard_instrument --out <dir> [--root <dir>] [options] <file-or-directory>...
//
// Every input file is copied to <out>/<path relative to root>, with MEMGUARD_FUNC("name");
// inserted at the start of each function body (same line, so line numbers are unchanged).
// Directories are scanned recursively for *.pwn, *.p and *.inc. A manifest listing every
// function (instrumented or skipped, with the reason) is written to <out>/memguard_instrument.txt.
//
// Then compile <out>/<your gamemode>.pwn instead of the original, with -i<out> placed before
// your other include paths so instrumented copies of your own includes are used.
#include "../src/instrument/pawn_instrumenter.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace memguard::instrument;

namespace {

void usage()
{
	std::cout <<
		"usage: memguard_instrument --out <dir> [--root <dir>] [options] <file-or-dir>...\n"
		"options:\n"
		"  --out <dir>        output directory (required; must not be the source directory)\n"
		"  --root <dir>       base directory for mirroring paths (default: common parent of inputs)\n"
		"  --exclude <name>   do not instrument this function (wildcards: *), repeatable\n"
		"  --only <name>      instrument only matching functions (wildcards: *), repeatable\n"
		"  --only-public      instrument only public functions (callbacks, timers, commands)\n"
		"  --prefix <text>    prefix for zone names (e.g. gm:)\n"
		"  --allow-emit       also instrument functions that contain #emit / __emit\n"
		"  --dry-run          analyse and print, write nothing\n"
		"  --quiet            only print the summary\n";
}

bool isSource(const fs::path& p)
{
	const std::string ext = p.extension().string();
	return ext == ".pwn" || ext == ".p" || ext == ".inc";
}

bool readFile(const fs::path& p, std::string& out)
{
	std::ifstream in(p, std::ios::binary);
	if (!in)
	{
		return false;
	}
	std::ostringstream ss;
	ss << in.rdbuf();
	out = ss.str();
	return true;
}

bool isInside(const fs::path& child, const fs::path& parent)
{
	const auto rel = child.lexically_relative(parent);
	return !rel.empty() && rel.begin()->string() != "..";
}

} // namespace

int main(int argc, char** argv)
{
	Options options;
	fs::path outDir;
	fs::path root;
	bool dryRun = false;
	bool quiet = false;
	std::vector<fs::path> inputs;

	for (int i = 1; i < argc; ++i)
	{
		const std::string a = argv[i];
		auto next = [&](const char* what) -> std::string {
			if (i + 1 >= argc)
			{
				std::cerr << "missing value for " << what << "\n";
				std::exit(2);
			}
			return argv[++i];
		};
		if (a == "--out")
			outDir = next("--out");
		else if (a == "--root")
			root = next("--root");
		else if (a == "--exclude")
			options.exclude.push_back(next("--exclude"));
		else if (a == "--only")
			options.include.push_back(next("--only"));
		else if (a == "--only-public")
			options.onlyPublic = true;
		else if (a == "--prefix")
			options.prefix = next("--prefix");
		else if (a == "--allow-emit")
			options.allowEmit = true;
		else if (a == "--dry-run")
			dryRun = true;
		else if (a == "--quiet")
			quiet = true;
		else if (a == "-h" || a == "--help")
		{
			usage();
			return 0;
		}
		else if (!a.empty() && a[0] == '-')
		{
			std::cerr << "unknown option " << a << "\n";
			usage();
			return 2;
		}
		else
			inputs.emplace_back(a);
	}
	if (inputs.empty() || (outDir.empty() && !dryRun))
	{
		usage();
		return 2;
	}

	// Expand directories.
	std::vector<fs::path> files;
	for (const fs::path& in : inputs)
	{
		std::error_code ec;
		if (fs::is_directory(in, ec))
		{
			for (const auto& e : fs::recursive_directory_iterator(in, ec))
			{
				if (e.is_regular_file() && isSource(e.path()) && e.path().filename() != "memguard.inc")
				{
					files.push_back(fs::absolute(e.path()).lexically_normal());
				}
			}
		}
		else if (fs::is_regular_file(in, ec))
		{
			files.push_back(fs::absolute(in).lexically_normal());
		}
		else
		{
			std::cerr << "error: input not found: " << in.string() << "\n";
			return 2;
		}
	}
	if (files.empty())
	{
		std::cerr << "error: no .pwn/.p/.inc files found\n";
		return 2;
	}

	if (root.empty())
	{
		root = files.front().parent_path();
		for (const fs::path& f : files)
		{
			while (!isInside(f, root) && root.has_parent_path() && root != root.parent_path())
			{
				root = root.parent_path();
			}
		}
	}
	root = fs::absolute(root).lexically_normal();
	if (!outDir.empty())
	{
		outDir = fs::absolute(outDir).lexically_normal();
		if (outDir == root)
		{
			std::cerr << "error: --out must differ from the source directory (originals are never overwritten)\n";
			return 2;
		}
	}

	std::ostringstream manifest;
	manifest << "MemGuard X instrumentation manifest\nroot: " << root.string() << "\n\n";
	int totalInstrumented = 0;
	int totalSkipped = 0;
	int failedFiles = 0;
	bool anyIncludesMemguard = false;

	for (const fs::path& f : files)
	{
		if (!isInside(f, root))
		{
			std::cerr << "error: " << f.string() << " is outside --root " << root.string() << "\n";
			return 2;
		}
		const fs::path rel = f.lexically_relative(root);
		std::string text;
		if (!readFile(f, text))
		{
			std::cerr << "error: cannot read " << f.string() << "\n";
			++failedFiles;
			continue;
		}
		const Result r = instrumentSource(text, options);
		if (!r.ok)
		{
			std::cerr << "error: " << rel.string() << ":" << r.errorLine << ": " << r.error << " - file not instrumented\n";
			manifest << rel.string() << ": NOT INSTRUMENTED (" << r.error << " at line " << r.errorLine << ")\n";
			++failedFiles;
			continue;
		}
		anyIncludesMemguard = anyIncludesMemguard || r.includesMemguard;
		manifest << rel.string() << ": " << r.instrumentedCount() << " of " << r.functions.size() << " functions instrumented\n";
		for (const FunctionInfo& fn : r.functions)
		{
			manifest << "  line " << fn.line << "  " << (fn.instrumented ? "[profiled] " : "[skipped]  ") << fn.zone;
			if (!fn.instrumented)
			{
				manifest << "  (" << fn.skipReason << ")";
			}
			manifest << "\n";
			if (fn.instrumented)
				++totalInstrumented;
			else
				++totalSkipped;
		}
		if (!quiet)
		{
			std::cout << rel.string() << ": " << r.instrumentedCount() << "/" << r.functions.size() << " functions instrumented\n";
		}
		if (!dryRun)
		{
			const fs::path dest = outDir / rel;
			std::error_code ec;
			fs::create_directories(dest.parent_path(), ec);
			std::ofstream out(dest, std::ios::binary | std::ios::trunc);
			if (!out || !(out << r.output))
			{
				std::cerr << "error: cannot write " << dest.string() << "\n";
				return 1;
			}
		}
	}

	if (!dryRun)
	{
		std::ofstream m(outDir / "memguard_instrument.txt", std::ios::binary | std::ios::trunc);
		m << manifest.str();
	}
	if (!quiet && dryRun)
	{
		std::cout << "\n" << manifest.str();
	}
	std::cout << "MemGuard X instrumentation: " << files.size() << " file(s), " << totalInstrumented << " function(s) instrumented, "
			  << totalSkipped << " skipped, " << failedFiles << " file(s) failed"
			  << (dryRun ? " (dry run)" : ("; output in " + outDir.string())) << "\n";
	if (!anyIncludesMemguard && totalInstrumented > 0)
	{
		std::cout << "warning: none of the inputs contains #include <memguard>; MEMGUARD_FUNC is defined there - "
					 "add the include to your gamemode before compiling.\n";
	}
	return failedFiles == 0 ? 0 : 1;
}
