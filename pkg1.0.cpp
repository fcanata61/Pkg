// pkg_linux.cpp
// Single-file LFS-oriented package orchestrator (Linux-only).
// Requires: nlohmann/json.hpp (install libnlohmann-json-dev or drop header next to file).
//
// Compile:
//   g++ -std=c++20 pkg_linux.cpp -O2 -pthread -o pkg
//
// Usage examples:
//   REPO=./recipes SOURCES=./sources BIN=./bin LOGS=./logs ./pkg list
//   REPO=./recipes SOURCES=./sources BIN=./bin LOGS=./logs ./pkg --dry-run i bash
//   ./pkg --sync=https://github.com/you/recipes.git dl bash
//
// Recipe JSON structure (example):
/*
{
  "name": "bash",
  "version": "5.2",
  "deps": ["glibc","ncurses"],
  "sources": [
    "https://ftp.gnu.org/gnu/bash/bash-5.2.tar.gz"
  ],
  "sha256": "SINGLE_SHA256_FOR_FIRST_SOURCE_OR_FOR_TARBALL",
  "sha256s": ["sha256-for-src1", "sha256-for-src2"],
  "patches": [
    "https://example.org/bash-lfs.patch",
    "https://github.com/some/patches.git"
  ],
  "steps": [
    "./configure --prefix=/usr",
    "make -j$(nproc)",
    "make DESTDIR=${DESTDIR} install"
  ],
  "install_to_bin": true,
  "strip": true,
  "post_remove_hook": "echo removed ${PKG}"
}
*/

#include <bits/stdc++.h>
#include <filesystem>
#include <thread>
#include <atomic>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/utsname.h>
#include <nlohmann/json.hpp> // header-only, install or provide header locally

namespace fs = std::filesystem;
using json = nlohmann::json;
using clock_type = std::chrono::system_clock;

// ANSI colors
constexpr const char* C_RESET = "\033[0m";
constexpr const char* C_RED = "\033[1;31m";
constexpr const char* C_GREEN = "\033[1;32m";
constexpr const char* C_YELLOW = "\033[1;33m";
constexpr const char* C_BLUE = "\033[1;34m";
constexpr const char* C_MAG = "\033[1;35m";
constexpr const char* C_CYAN = "\033[1;36m";

static bool DRY_RUN = false;
static bool REVERSE_ORDER = true;
static bool GLOBAL_STRIP = false;
static bool INSTALL_TO_ROOT = false;

static inline bool is_linux() {
    struct utsname u;
    if (uname(&u) != 0) return false;
    std::string sys = u.sysname;
    return sys == "Linux";
}

static inline std::string now_str(){
    auto t = clock_type::to_time_t(clock_type::now());
    char buf[64];
    strftime(buf,sizeof(buf), "%Y-%m-%dT%H-%M-%S", localtime(&t));
    return std::string(buf);
}
static inline void ensure_dir(const fs::path &p){
    std::error_code ec;
    if (!fs::exists(p)) fs::create_directories(p, ec);
    if (ec) { std::cerr<<C_RED<<"[error] cannot create "<<p<<" : "<<ec.message()<<C_RESET<<"\n"; std::exit(1); }
}

static int run_simple(const std::string &cmd) {
    if (DRY_RUN) { std::cerr << C_CYAN << "$ " << cmd << C_RESET << "\n"; return 0; }
    int r = std::system(cmd.c_str());
    if (r != 0) std::cerr << C_RED << "[cmd exit " << r << "]\n" << C_RESET;
    return r;
}

// Spinner + redirect to logfile
static int run_cmd_spinner_log(const std::string &cmd, const fs::path &logfile) {
    if (DRY_RUN) { std::cerr << C_CYAN << "$ " << cmd << C_RESET << "\n"; return 0; }
    int fd = open(logfile.c_str(), O_CREAT|O_WRONLY|O_APPEND, 0644);
    if (fd<0) {
        std::cerr << C_YELLOW << "[warn] cannot open log " << logfile << C_RESET << "\n";
    }
    pid_t pid = fork();
    if (pid==0) {
        if (fd>=0) {
            dup2(fd, STDOUT_FILENO);
            dup2(fd, STDERR_FILENO);
        }
        execl("/bin/sh","sh","-lc",cmd.c_str(), (char*)NULL);
        _exit(127);
    } else if (pid<0) {
        if (fd>=0) close(fd);
        std::cerr << C_RED << "[error] fork failed\n" << C_RESET; return 127;
    } else {
        std::string spin = "|/-\\";
        size_t si = 0;
        int status=0;
        while (true) {
            int w = waitpid(pid, &status, WNOHANG);
            if (w==pid) break;
            std::cerr << "\r" << C_YELLOW << spin[si%spin.size()] << C_RESET << " " << cmd.substr(0,60) << "..." << std::flush;
            si++; std::this_thread::sleep_for(std::chrono::milliseconds(120));
        }
        std::cerr << "\r" << std::string(80,' ') << "\r";
        if (fd>=0) close(fd);
        if (WIFEXITED(status)) {
            int code = WEXITSTATUS(status);
            if (code!=0) std::cerr << C_RED << "[cmd exit " << code << "] log: " << logfile << C_RESET << "\n";
            return code;
        } else {
            std::cerr << C_RED << "[cmd aborted]" << C_RESET << "\n";
            return 1;
        }
    }
}

// small helpers
static std::string filename_from_url(const std::string &url) {
    auto q = url.find('?');
    std::string u = (q==std::string::npos)?url:url.substr(0,q);
    auto p = u.find_last_of('/');
    if (p==std::string::npos) return u;
    std::string nm = u.substr(p+1);
    if (nm.empty()) nm = "downloaded";
    return nm;
}

// Recipe structure
struct Recipe {
    std::string name;
    std::string version;
    std::vector<std::string> deps;
    std::vector<std::string> sources;
    std::vector<std::string> sha256s; // optional - one per source or single sha256 in single element
    std::vector<std::string> patches;
    std::vector<std::string> steps;
    bool install_to_bin = true;
    bool strip = false;
    std::string post_remove_hook;
    fs::path path;
};

// parse one JSON file
static std::optional<Recipe> parse_recipe(const fs::path &p) {
    try {
        std::ifstream ifs(p);
        if (!ifs) return std::nullopt;
        json j = json::parse(ifs);
        Recipe r; r.path = p;
        if (j.contains("name")) r.name = j["name"].get<std::string>();
        else r.name = p.stem().string();
        if (j.contains("version")) r.version = j["version"].get<std::string>();
        if (j.contains("deps")) {
            for (auto &e : j["deps"]) r.deps.push_back(e.get<std::string>());
        }
        if (j.contains("sources")) {
            for (auto &e : j["sources"]) r.sources.push_back(e.get<std::string>());
        }
        if (j.contains("sha256")) {
            r.sha256s.push_back(j["sha256"].get<std::string>());
        }
        if (j.contains("sha256s")) {
            for (auto &e : j["sha256s"]) r.sha256s.push_back(e.get<std::string>());
        }
        if (j.contains("patches")) {
            for (auto &e : j["patches"]) r.patches.push_back(e.get<std::string>());
        }
        if (j.contains("steps")) {
            for (auto &e : j["steps"]) r.steps.push_back(e.get<std::string>());
        }
        if (j.contains("install_to_bin")) r.install_to_bin = j["install_to_bin"].get<bool>();
        if (j.contains("strip")) r.strip = j["strip"].get<bool>();
        if (j.contains("post_remove_hook")) r.post_remove_hook = j["post_remove_hook"].get<std::string>();
        return r;
    } catch (std::exception &ex) {
        std::cerr << C_RED << "[error] parsing recipe " << p << ": " << ex.what() << C_RESET << "\n";
        return std::nullopt;
    }
}

static std::vector<Recipe> load_recipes(const fs::path &REPO) {
    std::vector<Recipe> out;
    if (!fs::exists(REPO)) return out;
    for (auto &ent : fs::recursive_directory_iterator(REPO)) {
        if (!ent.is_regular_file()) continue;
        auto p = ent.path();
        if (p.extension()==".json" || p.extension()==".recipe" || p.extension()==".rcp") {
            auto maybe = parse_recipe(p);
            if (maybe) out.push_back(*maybe);
        }
    }
    return out;
}
static std::unordered_map<std::string,int> index_recipes(const std::vector<Recipe> &recipes){
    std::unordered_map<std::string,int> m;
    for (size_t i=0;i<recipes.size();++i) m[recipes[i].name] = (int)i;
    return m;
}

// topological sort (Kahn)
static std::vector<int> topo_sort(const std::vector<Recipe>& recipes) {
    int n = (int)recipes.size();
    auto idx = index_recipes(recipes);
    std::vector<int> indeg(n,0);
    std::vector<std::vector<int>> adj(n);
    for (int i=0;i<n;++i) {
        for (auto &d : recipes[i].deps) {
            auto it = idx.find(d);
            if (it!=idx.end()) {
                int j = it->second;
                adj[j].push_back(i);
                indeg[i]++;
            } else {
                std::cerr << C_YELLOW << "[warn] dep '"<<d<<"' of '"<<recipes[i].name<<"' not found - assumed external\n"<<C_RESET;
            }
        }
    }
    std::deque<int> q;
    for (int i=0;i<n;++i) if (indeg[i]==0) q.push_back(i);
    std::vector<int> order;
    while (!q.empty()) {
        int u = q.front(); q.pop_front();
        order.push_back(u);
        for (int v: adj[u]) {
            indeg[v]--;
            if (indeg[v]==0) q.push_back(v);
        }
    }
    if ((int)order.size()!=n) {
        std::cerr << C_RED << "[error] cycle detected in recipes\n" << C_RESET;
        return {};
    }
    return order;
}

// download via curl
static int curl_download(const std::string &url, const fs::path &dest, const fs::path &log) {
    ensure_dir(dest.parent_path());
    if (fs::exists(dest)) {
        std::cerr << C_YELLOW << "[info] cached: " << dest << C_RESET << "\n";
        return 0;
    }
    std::string cmd = "curl -L --fail --retry 3 -o " + dest.string() + " " + url;
    return run_cmd_spinner_log(cmd, log);
}

// git clone/pull
static int git_fetch(const std::string &url, const fs::path &dest, const fs::path &log) {
    ensure_dir(dest.parent_path());
    if (fs::exists(dest) && fs::exists(dest/".git")) {
        std::string cmd = "git -C " + dest.string() + " pull --rebase --autostash";
        return run_cmd_spinner_log(cmd, log);
    } else {
        std::string cmd = "git clone --depth 1 " + url + " " + dest.string();
        return run_cmd_spinner_log(cmd, log);
    }
}

// apply a patch file
static int apply_patch_file(const fs::path &patchfile, const fs::path &workdir, const fs::path &log) {
    std::string cmd = "cd " + workdir.string() + " && patch -p1 < " + patchfile.string();
    return run_cmd_spinner_log(cmd, log);
}

// prepare workdir from sources
static fs::path prepare_workdir(const Recipe &r, const fs::path &SOURCES, const fs::path &WORK, const fs::path &LOGS) {
    std::string ts = now_str();
    fs::path wd = WORK / (r.name + "-" + ts);
    ensure_dir(wd);
    // prefer git sources
    for (auto &src : r.sources) {
        if (src.find(".git")!=std::string::npos) {
            fs::path gitdest = SOURCES / (r.name + "-git");
            int g = git_fetch(src, gitdest, LOGS / (r.name + ".git.log"));
            if (g!=0) { std::cerr<<C_RED<<"[error] git fetch failed\n"<<C_RESET; break; }
            run_simple("cp -a " + gitdest.string() + "/. " + wd.string());
            return wd;
        }
    }
    // else tarballs
    for (auto &src : r.sources) {
        std::string fn = filename_from_url(src);
        fs::path dest = SOURCES / fn;
        if (!fs::exists(dest)) {
            int d = curl_download(src, dest, LOGS / (r.name + ".download.log"));
            if (d!=0) { std::cerr<<C_RED<<"[error] download failed\n"<<C_RESET; continue; }
        }
        // try extract strip 1
        int ex = run_simple("tar -xf " + dest.string() + " -C " + wd.string() + " --strip-components=1 2>/dev/null");
        if (ex!=0) run_simple("tar -xf " + dest.string() + " -C " + wd.string());
        return wd;
    }
    // fallback: copy recipe folder
    if (!r.path.empty()) {
        fs::path repo_dir = r.path.parent_path();
        if (fs::exists(repo_dir)) {
            run_simple("cp -a " + repo_dir.string() + "/. " + wd.string());
            return wd;
        }
    }
    return wd;
}

// apply patches (supports git repos and plain URLs)
static int apply_patches(const Recipe &r, const fs::path &workdir, const fs::path &SOURCES, const fs::path &LOGS) {
    if (r.patches.empty()) return 0;
    int acc = 0;
    for (auto &p : r.patches) {
        if (p.find(".git")!=std::string::npos) {
            fs::path tmp = SOURCES / ("patches-" + r.name);
            acc |= git_fetch(p, tmp, LOGS / (r.name + ".patches.git.log"));
            for (auto &e : fs::directory_iterator(tmp)) {
                if (!e.is_regular_file()) continue;
                acc |= apply_patch_file(e.path(), workdir, LOGS / (r.name + ".patches.log"));
            }
        } else {
            std::string fn = filename_from_url(p);
            fs::path dest = SOURCES / fn;
            acc |= curl_download(p, dest, LOGS / (r.name + ".patches.download.log"));
            acc |= apply_patch_file(dest, workdir, LOGS / (r.name + ".patches.log"));
        }
    }
    return acc;
}

// execute build steps
static int execute_steps(const Recipe &r, const fs::path &workdir, const fs::path &LOGS) {
    ensure_dir(LOGS);
    fs::path logfile = LOGS / (r.name + ".build.log");
    fs::path destdir = workdir / "DESTDIR";
    ensure_dir(destdir);
    for (auto &step : r.steps) {
        std::string cmd = "DESTDIR=" + destdir.string() + " " + step;
        std::string full = "cd " + workdir.string() + " && " + cmd;
        int rc = run_cmd_spinner_log(full, logfile);
        if (rc!=0) return rc;
    }
    return 0;
}

// package DESTDIR into tar.gz
static fs::path package_dest(const Recipe &r, const fs::path &workdir, const fs::path &PACKAGES) {
    ensure_dir(PACKAGES);
    std::string pkgname = r.name + "-" + (r.version.empty()?now_str():r.version) + ".tar.gz";
    fs::path out = PACKAGES / pkgname;
    fs::path destdir = workdir / "DESTDIR";
    int rc = run_simple("tar -czf " + out.string() + " -C " + destdir.string() + " .");
    if (rc!=0) std::cerr<<C_YELLOW<<"[warn] packaging failed for "<<r.name<<C_RESET<<"\n";
    return out;
}

// strip binaries under DESTDIR
static void strip_binaries(const fs::path &workdir) {
    fs::path destdir = workdir / "DESTDIR";
    std::vector<fs::path> candidates;
    if (fs::exists(destdir / "usr" / "bin")) candidates.push_back(destdir / "usr" / "bin");
    if (fs::exists(destdir / "bin")) candidates.push_back(destdir / "bin");
    for (auto &p : candidates) {
        for (auto &e : fs::recursive_directory_iterator(p)) {
            if (!e.is_regular_file()) continue;
            run_simple("strip --strip-all " + e.path().string() + " 2>/dev/null || true");
        }
    }
}

// install: copy files and create manifest
static int install_from_destdir(const Recipe &r, const fs::path &workdir, const fs::path &BIN, const fs::path &LOGS, fs::path &manifest_out) {
    fs::path destdir = workdir / "DESTDIR";
    if (!fs::exists(destdir)) { std::cerr<<C_RED<<"[error] no DESTDIR\n"<<C_RESET; return 1; }
    fs::path target = INSTALL_TO_ROOT ? fs::path("/") : BIN;
    ensure_dir(target);
    std::vector<std::string> manifest;
    for (auto &e : fs::recursive_directory_iterator(destdir)) {
        if (!e.is_regular_file()) continue;
        fs::path rel = fs::relative(e.path(), destdir);
        fs::path dest = target / rel;
        ensure_dir(dest.parent_path());
        try {
            fs::copy_file(e.path(), dest, fs::copy_options::overwrite_existing);
            manifest.push_back(dest.string());
        } catch (std::exception &ex) {
            std::cerr<<C_YELLOW<<"[warn] copy failed: "<<ex.what()<<C_RESET<<"\n";
        }
    }
    fs::path state = LOGS / "state";
    ensure_dir(state);
    fs::path mf = state / (r.name + ".manifest");
    std::ofstream ofs(mf);
    for (auto &f : manifest) ofs << f << "\n";
    ofs.close();
    manifest_out = mf;
    return 0;
}

// remove package by manifest
static int remove_by_manifest(const Recipe &r, const fs::path &LOGS) {
    fs::path mf = LOGS / "state" / (r.name + ".manifest");
    if (!fs::exists(mf)) { std::cerr<<C_YELLOW<<"[info] manifest not found\n"<<C_RESET; return 0; }
    std::vector<std::string> files;
    std::ifstream ifs(mf);
    std::string line;
    while (std::getline(ifs,line)) if (!line.empty()) files.push_back(line);
    int acc = 0;
    for (auto &f : files) {
        try {
            if (fs::exists(f)) {
                fs::remove(f);
                std::cerr<<C_CYAN<<"[removed] "<<f<<C_RESET<<"\n";
            }
        } catch (...) { std::cerr<<C_YELLOW<<"[warn] couldn't remove "<<f<<C_RESET<<"\n"; acc=1; }
    }
    try { fs::remove(mf); } catch(...) {}
    return acc;
}

// validate sha256 via system sha256sum: returns 0 if matches, nonzero otherwise
static int validate_sha256(const fs::path &file, const std::string &expected) {
    if (!fs::exists(file)) return 2;
    // run "sha256sum <file>" and parse first token
    std::string cmd = "sha256sum " + file.string() + " 2>/dev/null | awk '{print $1}'";
    FILE *fp = popen(cmd.c_str(), "r");
    if (!fp) return 3;
    char buf[512]; buf[0]=0;
    if (fgets(buf, sizeof(buf), fp)==NULL) { pclose(fp); return 4; }
    pclose(fp);
    std::string got = trim(std::string(buf));
    if (got == expected) return 0;
    std::cerr << C_RED << "[sha256 mismatch] expected " << expected << " got " << got << C_RESET << "\n";
    return 5;
}

// trim helper
static std::string trim(const std::string &s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a==std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a,b-a+1);
}

// prepare & build & package & install orchestration for one recipe
static int process_recipe(const Recipe &r,
                          const fs::path &REPO, const fs::path &SOURCES,
                          const fs::path &BIN, const fs::path &LOGS,
                          bool build_only, bool do_package, bool do_install, bool strip_opt) {
    std::cerr<<C_MAG<<"=== "<<r.name<<" ==="<<C_RESET<<"\n";
    ensure_dir(SOURCES); ensure_dir(BIN); ensure_dir(LOGS);
    fs::path workroot = fs::temp_directory_path() / "lfs-pkg-work";
    ensure_dir(workroot);
    fs::path wd = prepare_workdir(r, SOURCES, workroot, LOGS);
    std::cerr<<C_CYAN<<"[workdir] "<<wd<<C_RESET<<"\n";
    // validate sha256s before extraction (if provided). We'll match sources order.
    for (size_t i=0;i<r.sources.size();++i) {
        std::string src = r.sources[i];
        std::string fn = filename_from_url(src);
        fs::path dest = SOURCES / fn;
        if (r.sha256s.size() > i && !r.sha256s[i].empty()) {
            if (!fs::exists(dest)) {
                int dl = curl_download(src, dest, LOGS / (r.name + ".download.log"));
                if (dl!=0) return dl;
            }
            int v = validate_sha256(dest, r.sha256s[i]);
            if (v!=0) return v;
        } else if (r.sha256s.size()==1 && !r.sha256s[0].empty() && r.sources.size()==1) {
            // single sha256 mapped to single source
            if (!fs::exists(dest)) {
                int dl = curl_download(src, dest, LOGS / (r.name + ".download.log"));
                if (dl!=0) return dl;
            }
            int v = validate_sha256(dest, r.sha256s[0]);
            if (v!=0) return v;
        }
    }
    int p = apply_patches(r, wd, SOURCES, LOGS);
    if (p!=0) { std::cerr<<C_RED<<"[error] patches failed\n"<<C_RESET; return p; }
    int b = execute_steps(r, wd, LOGS);
    if (b!=0) { std::cerr<<C_RED<<"[error] build failed\n"<<C_RESET; return b; }
    if (strip_opt || r.strip || GLOBAL_STRIP) strip_binaries(wd);
    fs::path packaged;
    if (do_package) {
        packaged = package_dest(r, wd, SOURCES / "packages");
        std::cerr<<C_GREEN<<"[packaged] "<<packaged<<C_RESET<<"\n";
    }
    if (do_install) {
        fs::path manifest;
        int ins = install_from_destdir(r, wd, BIN, LOGS, manifest);
        if (ins!=0) { std::cerr<<C_RED<<"[error] install failed\n"<<C_RESET; return ins; }
        std::cerr<<C_GREEN<<"[installed] manifest: "<<manifest<<C_RESET<<"\n";
    }
    return 0;
}

// sync repo via git
static int sync_repo(const fs::path &REPO, const std::string &url, const fs::path &LOGS) {
    if (!url.empty()) {
        if (fs::exists(REPO) && fs::exists(REPO/".git")) {
            std::string cmd = "git -C " + REPO.string() + " pull --rebase --autostash";
            return run_cmd_spinner_log(cmd, LOGS / "sync.log");
        } else {
            ensure_dir(REPO.parent_path());
            std::string cmd = "git clone --depth 1 " + url + " " + REPO.string();
            return run_cmd_spinner_log(cmd, LOGS / "sync.log");
        }
    } else {
        if (fs::exists(REPO) && fs::exists(REPO/".git")) {
            std::string cmd = "git -C " + REPO.string() + " pull --rebase --autostash";
            return run_cmd_spinner_log(cmd, LOGS / "sync.log");
        }
        std::cerr<<C_YELLOW<<"[info] not syncing (no URL)\n"<<C_RESET;
        return 0;
    }
}

// check installed status by manifest presence and files exist
static bool is_installed(const std::string &pkg, const fs::path &LOGS) {
    fs::path mf = LOGS / "state" / (pkg + ".manifest");
    if (!fs::exists(mf)) return false;
    std::ifstream ifs(mf);
    std::string line;
    while (std::getline(ifs,line)) {
        if (line.empty()) continue;
        if (fs::exists(line)) return true; // at least one file exists -> considered installed
    }
    return false;
}

// get installed binary path (first entry in manifest)
static std::string installed_path(const std::string &pkg, const fs::path &LOGS) {
    fs::path mf = LOGS / "state" / (pkg + ".manifest");
    if (!fs::exists(mf)) return "";
    std::ifstream ifs(mf);
    std::string line;
    while (std::getline(ifs,line)) {
        if (line.empty()) continue;
        return line;
    }
    return "";
}

// CLI
int main(int argc, char** argv) {
    if (!is_linux()) {
        std::cerr << C_RED << "[error] this program is Linux-only\n" << C_RESET;
        return 2;
    }

    fs::path REPO = fs::path(std::getenv("REPO")?std::getenv("REPO"):"./recipes");
    fs::path SOURCES = fs::path(std::getenv("SOURCES")?std::getenv("SOURCES"):"./sources");
    fs::path BIN = fs::path(std::getenv("BIN")?std::getenv("BIN"):"./bin");
    fs::path LOGS = fs::path(std::getenv("LOGS")?std::getenv("LOGS"):"./logs");
    std::string sync_url;

    if (argc<2) {
        std::cout << "Usage: pkg <cmd> [pkgname] [options]\nCommands: i/install, rm/remove, u/upgrade, dl/download, bp/build-only, log, list, info\nFlags: --dry-run --no-reverse --sync=URL --strip --root\n";
        return 1;
    }
    std::string cmd = argv[1];
    std::string pkgname = (argc>=3)?argv[2]:"";
    for (int i=1;i<argc;i++){
        std::string a = argv[i];
        if (a=="--dry-run") DRY_RUN=true;
        if (a=="--no-reverse") REVERSE_ORDER=false;
        if (a.rfind("--sync=",0)==0) sync_url = a.substr(7);
        if (a=="--strip") GLOBAL_STRIP=true;
        if (a=="--root") INSTALL_TO_ROOT=true;
    }
    if (cmd=="i") cmd="install";
    if (cmd=="rm") cmd="remove";
    if (cmd=="u") cmd="upgrade";
    if (cmd=="dl") cmd="download";
    if (cmd=="bp") cmd="build-only";

    std::cerr<<C_GREEN<<"pkg starting. REPO="<<REPO<<" SOURCES="<<SOURCES<<" BIN="<<BIN<<" LOGS="<<LOGS<<C_RESET<<"\n";
    ensure_dir(REPO); ensure_dir(SOURCES); ensure_dir(BIN); ensure_dir(LOGS);

    if (!sync_url.empty()) {
        int s = sync_repo(REPO, sync_url, LOGS);
        if (s!=0) std::cerr<<C_YELLOW<<"[warn] sync returned "<<s<<C_RESET<<"\n";
    }

    auto recipes = load_recipes(REPO);
    if (recipes.empty()) {
        std::cerr<<C_RED<<"[error] no recipes found under "<<REPO<<C_RESET<<"\n"; return 1;
    }
    auto idx = index_recipes(recipes);

    // list command
    if (cmd=="list") {
        std::cerr<<C_CYAN<<"Available recipes:\n"<<C_RESET;
        // sort by name
        std::sort(recipes.begin(), recipes.end(), [](const Recipe&a,const Recipe&b){return a.name<b.name;});
        for (auto &r: recipes) {
            bool inst = is_installed(r.name, LOGS);
            std::string mark = inst ? std::string("[\xE2\x9C\x94]") : "[ ]"; // ✔ or space
            std::cout << mark << " " << r.name << (r.version.empty() ? "" : (" " + r.version)) << "\n";
        }
        return 0;
    }

    // info command
    if (cmd=="info") {
        if (pkgname.empty()) { std::cerr<<C_YELLOW<<"Usage: pkg info <pkg>\n"<<C_RESET; return 1; }
        if (!idx.count(pkgname)) { std::cerr<<C_RED<<"[error] not found\n"<<C_RESET; return 1; }
        auto &r = recipes[idx[pkgname]];
        std::cout<<C_BLUE<<"Name: "<<C_RESET<<r.name<<"\n";
        std::cout<<C_BLUE<<"Version: "<<C_RESET<<(r.version.empty()?"(none)":r.version)<<"\n";
        std::cout<<C_BLUE<<"Deps: "<<C_RESET;
        if (r.deps.empty()) std::cout<<"(none)\n"; else { for (auto &d:r.deps) std::cout<<d<<" "; std::cout<<"\n"; }
        std::cout<<C_BLUE<<"Sources: "<<C_RESET;
        for (auto &s: r.sources) std::cout<<s<<" ";
        std::cout<<"\n";
        std::cout<<C_BLUE<<"SHA256s: "<<C_RESET;
        for (auto &h: r.sha256s) std::cout<<h<<" ";
        std::cout<<"\n";
        bool inst = is_installed(r.name, LOGS);
        std::cout<<C_BLUE<<"Status: "<<C_RESET<<(inst?C_GREEN "installed" C_RESET : C_YELLOW "not installed" C_RESET)<<"\n";
        if (inst) {
            std::string p = installed_path(r.name, LOGS);
            if (!p.empty()) std::cout<<C_BLUE<<"Installed path (example): "<<C_RESET<<p<<"\n";
            fs::path mf = LOGS / "state" / (r.name + ".manifest");
            std::cout<<C_BLUE<<"Manifest: "<<C_RESET<<mf<<"\n";
        }
        std::cout<<C_BLUE<<"Logs: "<<C_RESET<<(LOGS / (r.name + ".build.log"))<<"\n";
        return 0;
    }

    // determine target set (pkgname or all)
    std::vector<Recipe> selected;
    if (pkgname.empty()) {
        selected = recipes;
    } else {
        if (!idx.count(pkgname)) { std::cerr<<C_RED<<"[error] not found\n"<<C_RESET; return 1; }
        // gather requested and transitive deps
        std::set<int> chosen;
        std::function<void(int)> dfs = [&](int u){
            if (chosen.count(u)) return; chosen.insert(u);
            for (auto &d : recipes[u].deps) if (idx.count(d)) dfs(idx[d]);
        };
        dfs(idx[pkgname]);
        for (int i: chosen) selected.push_back(recipes[i]);
    }

    // topo sort among selected
    std::set<std::string> selnames; for (auto &r:selected) selnames.insert(r.name);
    std::vector<Recipe> pool;
    for (auto &r: recipes) if (selnames.count(r.name)) pool.push_back(r);
    auto order_idx = topo_sort(pool);
    if (order_idx.empty()) return 2;
    std::vector<int> exec_order = order_idx;
    if (REVERSE_ORDER) std::reverse(exec_order.begin(), exec_order.end());

    std::cerr<<C_CYAN<<"Planned sequence:\n"<<C_RESET;
    for (int id: exec_order) std::cerr<<" - "<<pool[id].name<<"\n";

    // commands
    if (cmd=="download" || cmd=="dl") {
        for (int id : exec_order) {
            auto &r = pool[id];
            std::cerr<<C_MAG<<"[download] "<<r.name<<C_RESET<<"\n";
            for (auto &s : r.sources) {
                std::string fn = filename_from_url(s);
                fs::path dest = SOURCES / fn;
                int rc = curl_download(s, dest, LOGS / (r.name + ".download.log"));
                if (rc!=0) return rc;
            }
            for (auto &p : r.patches) {
                if (p.find(".git")!=std::string::npos) {
                    fs::path tmp = SOURCES / ("patches-" + r.name);
                    int rc = git_fetch(p, tmp, LOGS / (r.name + ".patches.git.log"));
                    if (rc!=0) return rc;
                } else {
                    std::string fn = filename_from_url(p);
                    fs::path dest = SOURCES / fn;
                    int rc = curl_download(p, dest, LOGS / (r.name + ".patches.download.log"));
                    if (rc!=0) return rc;
                }
            }
        }
        std::cerr<<C_GREEN<<"download complete\n"<<C_RESET; return 0;
    } else if (cmd=="build-only" || cmd=="bp") {
        for (int id : exec_order) {
            auto &r = pool[id];
            int rc = process_recipe(r, REPO, SOURCES, BIN, LOGS, true, true, false, GLOBAL_STRIP);
            if (rc!=0) return rc;
        }
        return 0;
    } else if (cmd=="install" || cmd=="i") {
        for (int id: exec_order) {
            auto &r = pool[id];
            int rc = process_recipe(r, REPO, SOURCES, BIN, LOGS, false, true, true, GLOBAL_STRIP);
            if (rc!=0) return rc;
        }
        return 0;
    } else if (cmd=="remove" || cmd=="rm") {
        for (int id : exec_order) {
            auto &r = pool[id];
            std::cerr<<C_MAG<<"[remove] "<<r.name<<C_RESET<<"\n";
            int rc = remove_by_manifest(r, LOGS);
            if (rc!=0) std::cerr<<C_YELLOW<<"[warn] some files couldn't be removed\n"<<C_RESET;
            if (!r.post_remove_hook.empty()) {
                std::string hook = r.post_remove_hook;
                size_t pos;
                while ((pos = hook.find("${PKG}")) != std::string::npos) hook.replace(pos,6,r.name);
                run_simple(hook);
            }
        }
        return 0;
    } else if (cmd=="upgrade" || cmd=="u") {
        for (int id : exec_order) {
            auto &r = pool[id];
            std::cerr<<C_MAG<<"[upgrade] "<<r.name<<C_RESET<<"\n";
            int rc = process_recipe(r, REPO, SOURCES, BIN, LOGS, false, true, true, GLOBAL_STRIP);
            if (rc!=0) return rc;
        }
        return 0;
    } else if (cmd=="log") {
        if (pkgname.empty()) { std::cerr<<C_YELLOW<<"Usage: pkg log <pkg>\n"<<C_RESET; return 1; }
        fs::path l = LOGS / (pkgname + ".build.log");
        if (!fs::exists(l)) l = LOGS / (pkgname + ".download.log");
        if (!fs::exists(l)) { std::cerr<<C_YELLOW<<"no log found\n"<<C_RESET; return 1; }
        std::ifstream ifs(l);
        std::string line;
        while (std::getline(ifs,line)) std::cout<<line<<"\n";
        return 0;
    }

    std::cerr<<C_YELLOW<<"[error] unknown command: "<<cmd<<C_RESET<<"\n";
    return 1;
}
