// pkg.cpp
// Single-file LFS-oriented package orchestrator (prototype).
// Compile: g++ -std=c++20 pkg.cpp -O2 -pthread -o pkg
//
// Usage (examples):
//   REPO=./recipes SOURCES=./sources BIN=./bin LOGS=./logs ./pkg --dry-run i bash
//   ./pkg --sync=https://github.com/you/recipes.git dl bash
//
// Recipe format (JSON) - require exact keys (simple):
/*
{
  "name": "bash",
  "version": "5.2",
  "deps": ["glibc","ncurses"],
  "sources": [
    "https://ftp.gnu.org/gnu/bash/bash-5.2.tar.gz"
  ],
  "patches": [
    "https://example.org/bash-lfs.patch",
    "https://github.com/some/patches.git"   // git repo allowed
  ],
  "steps": [
    "./configure --prefix=/usr",
    "make -j$(nproc)",
    "make check",
    "make DESTDIR=${DESTDIR} install"
  ],
  "install_to_bin": true,
  "strip": true,
  "post_remove_hook": "echo 'removed ${PKG}'"
}
*/

#include <bits/stdc++.h>
#include <filesystem>
#include <thread>
#include <atomic>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>
namespace fs = std::filesystem;

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

// Simple JSON "lite" parser tailored to our recipe schema.
// It's minimal and permissive, expects reasonably formatted JSON.
// This avoids external libs for a single-file deliverable.
struct JValue {
    enum Type { NULLV, BOOLV, NUM, STR, ARR, OBJ } t = NULLV;
    bool b = false;
    long double num = 0;
    std::string s;
    std::vector<JValue> a;
    std::map<std::string,JValue> o;
    JValue() = default;
    static JValue make_str(const std::string& x){ JValue v; v.t=STR; v.s=x; return v;}
    static JValue make_bool(bool x){ JValue v; v.t=BOOLV; v.b=x; return v;}
    static JValue make_null(){ JValue v; v.t=NULLV; return v;}
};

// Trim helpers
static inline std::string trim(const std::string &s){
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a==std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a,b-a+1);
}

// Very small JSON tokenizer/parser. Not fully spec-compliant but OK for config files.
// Supports strings with escapes \" and unicode not handled.
class SimpleJSON {
public:
    SimpleJSON(const std::string &src): src(src), i(0), n(src.size()){}
    JValue parse() {
        skip_ws();
        return parse_value();
    }
private:
    std::string src;
    size_t i, n;
    void skip_ws(){ while (i<n && isspace((unsigned char)src[i])) ++i; }
    bool match(char c){ skip_ws(); if (i<n && src[i]==c){ ++i; return true;} return false; }
    JValue parse_value(){
        skip_ws();
        if (i>=n) return JValue::make_null();
        char c = src[i];
        if (c=='"') return parse_string();
        if (c=='{') return parse_object();
        if (c=='[') return parse_array();
        if (c=='t' || c=='f') return parse_bool();
        if (c=='n') return parse_null();
        // number?
        if (c=='-' || (c>='0' && c<='9')) return parse_number();
        return JValue::make_null();
    }
    JValue parse_string(){
        // assume src[i]=='"'
        ++i;
        std::string out;
        while (i<n){
            char c = src[i++];
            if (c=='"') break;
            if (c=='\\' && i<n){
                char e = src[i++];
                if (e=='n') out.push_back('\n');
                else if (e=='t') out.push_back('\t');
                else if (e=='r') out.push_back('\r');
                else if (e=='\\') out.push_back('\\');
                else if (e=='"') out.push_back('"');
                else out.push_back(e);
            } else {
                out.push_back(c);
            }
        }
        return JValue::make_str(out);
    }
    JValue parse_number(){
        size_t start = i;
        if (src[i]=='-') ++i;
        while (i<n && isdigit((unsigned char)src[i])) ++i;
        if (i<n && src[i]=='.'){
            ++i;
            while (i<n && isdigit((unsigned char)src[i])) ++i;
        }
        std::string num = src.substr(start, i-start);
        JValue v; v.t = JValue::NUM; v.num = strtold(num.c_str(), nullptr); return v;
    }
    JValue parse_bool(){
        if (src.compare(i,4,"true")==0){ i+=4; return JValue::make_bool(true); }
        if (src.compare(i,5,"false")==0){ i+=5; return JValue::make_bool(false); }
        return JValue::make_null();
    }
    JValue parse_null(){
        if (src.compare(i,4,"null")==0){ i+=4; return JValue::make_null(); }
        return JValue::make_null();
    }
    JValue parse_array(){
        ++i; // skip [
        JValue arr; arr.t = JValue::ARR;
        skip_ws();
        if (i<n && src[i]==']'){ ++i; return arr; }
        while (i<n){
            JValue v = parse_value();
            arr.a.push_back(v);
            skip_ws();
            if (i<n && src[i]==','){ ++i; continue; }
            if (i<n && src[i]==']'){ ++i; break; }
        }
        return arr;
    }
    JValue parse_object(){
        ++i; // skip {
        JValue obj; obj.t = JValue::OBJ;
        skip_ws();
        if (i<n && src[i]=='}'){ ++i; return obj; }
        while (i<n){
            skip_ws();
            if (i<n && src[i]=='"'){
                JValue key = parse_string();
                skip_ws();
                if (i<n && src[i]==':') ++i;
                JValue val = parse_value();
                obj.o[key.s] = val;
            }
            skip_ws();
            if (i<n && src[i]==','){ ++i; continue; }
            if (i<n && src[i]=='}'){ ++i; break; }
        }
        return obj;
    }
};

// Recipe structure
struct Recipe {
    std::string name;
    std::string version;
    std::vector<std::string> deps;
    std::vector<std::string> sources;
    std::vector<std::string> patches;
    std::vector<std::string> steps;
    bool install_to_bin = true;
    bool strip = false;
    std::string post_remove_hook;
    fs::path path; // file path
};

// Parse recipe JSON file into Recipe struct
std::optional<Recipe> parse_recipe(const fs::path &p) {
    std::ifstream ifs(p);
    if (!ifs) return std::nullopt;
    std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    SimpleJSON parser(content);
    JValue root = parser.parse();
    if (root.t != JValue::OBJ) return std::nullopt;
    Recipe r; r.path = p;
    auto get_str = [&](const std::string &k)->std::string{
        auto it = root.o.find(k);
        if (it==root.o.end()) return "";
        if (it->second.t==JValue::STR) return it->second.s;
        if (it->second.t==JValue::NUM) return std::to_string((long long)it->second.num);
        return "";
    };
    auto get_bool = [&](const std::string &k, bool def=false)->bool{
        auto it = root.o.find(k);
        if (it==root.o.end()) return def;
        if (it->second.t==JValue::BOOLV) return it->second.b;
        if (it->second.t==JValue::STR) {
            std::string s = it->second.s;
            std::transform(s.begin(), s.end(), s.begin(), ::tolower);
            return s=="true"||s=="1"||s=="yes";
        }
        return def;
    };
    auto get_arr = [&](const std::string &k)->std::vector<std::string>{
        std::vector<std::string> out;
        auto it = root.o.find(k);
        if (it==root.o.end()) return out;
        if (it->second.t==JValue::ARR) {
            for (auto &el: it->second.a) {
                if (el.t==JValue::STR) out.push_back(el.s);
            }
        }
        return out;
    };
    r.name = get_str("name");
    if (r.name.empty()) r.name = p.stem().string();
    r.version = get_str("version");
    r.deps = get_arr("deps");
    r.sources = get_arr("sources");
    r.patches = get_arr("patches");
    r.steps = get_arr("steps");
    r.install_to_bin = get_bool("install_to_bin", true);
    r.strip = get_bool("strip", false);
    r.post_remove_hook = get_str("post_remove_hook");
    return r;
}

// Helpers
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

// run command with stdout/stderr redirected to logfile, show spinner while running
int run_command_with_spinner(const std::string &cmd, const fs::path &logfile) {
    if (DRY_RUN) {
        std::cerr << C_CYAN << "$ " << cmd << C_RESET << "\n";
        return 0;
    }
    // open logfile
    int fd = open(logfile.c_str(), O_CREAT|O_WRONLY|O_APPEND, 0644);
    if (fd<0) {
        std::cerr << C_YELLOW << "[warn] cannot open log " << logfile << C_RESET << "\n";
    }
    pid_t pid = fork();
    if (pid == 0) {
        // child
        if (fd>=0) {
            dup2(fd, STDOUT_FILENO);
            dup2(fd, STDERR_FILENO);
        }
        // set env to produce simpler output
        execl("/bin/sh", "sh", "-lc", cmd.c_str(), (char*)NULL);
        _exit(127);
    } else if (pid < 0) {
        if (fd>=0) close(fd);
        std::cerr << C_RED << "[error] fork failed\n" << C_RESET;
        return 127;
    } else {
        // parent: spinner
        std::string spin = "|/-\\";
        size_t si = 0;
        int status = 0;
        while (true) {
            int r = waitpid(pid, &status, WNOHANG);
            if (r == pid) break;
            // print spinner
            std::cerr << "\r" << C_YELLOW << spin[si%spin.size()] << C_RESET << " " << cmd.substr(0,60) << "..." << std::flush;
            si++;
            std::this_thread::sleep_for(std::chrono::milliseconds(120));
        }
        std::cerr << "\r" << std::string(80,' ') << "\r"; // clear line
        if (fd>=0) close(fd);
        if (WIFEXITED(status)) {
            int code = WEXITSTATUS(status);
            if (code != 0) std::cerr << C_RED << "[cmd exit " << code << "] see " << logfile << C_RESET << "\n";
            return code;
        } else {
            std::cerr << C_RED << "[cmd aborted]" << C_RESET << "\n";
            return 1;
        }
    }
}

// run simple command (no spinner) - used rarely
int run_simple(const std::string &cmd) {
    if (DRY_RUN) { std::cerr << C_CYAN << "$ " << cmd << C_RESET << "\n"; return 0; }
    int r = std::system(cmd.c_str());
    if (r != 0) std::cerr << C_RED << "[cmd exit " << r << "]\n" << C_RESET;
    return r;
}

// Read all recipe files under REPO (extension .json)
std::vector<Recipe> load_recipes(const fs::path &REPO) {
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

std::unordered_map<std::string,int> index_recipes(const std::vector<Recipe> &recipes){
    std::unordered_map<std::string,int> m;
    for (size_t i=0;i<recipes.size();++i) m[recipes[i].name] = (int)i;
    return m;
}

// Topo sort Kahn
std::vector<int> topo_sort(const std::vector<Recipe>& recipes) {
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
                std::cerr << C_YELLOW << "[warn] dep '"<<d<<"' of '"<<recipes[i].name<<"' not found in recipes, treating external\n"<<C_RESET;
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

// util: filename from url
std::string filename_from_url(const std::string &url) {
    auto q = url.find('?');
    std::string u = (q==std::string::npos)?url:url.substr(0,q);
    auto p = u.find_last_of('/');
    if (p==std::string::npos) return u;
    std::string nm = u.substr(p+1);
    if (nm.empty()) nm = "downloaded";
    return nm;
}

// git clone or pull
int git_fetch(const std::string &url, const fs::path &dest, const fs::path &log) {
    ensure_dir(dest.parent_path());
    if (fs::exists(dest) && fs::exists(dest/".git")) {
        std::string cmd = "git -C " + dest.string() + " pull --rebase --autostash";
        return run_command_with_spinner(cmd, log);
    } else {
        std::string cmd = "git clone --depth 1 " + url + " " + dest.string();
        return run_command_with_spinner(cmd, log);
    }
}

// curl download
int curl_download(const std::string &url, const fs::path &dest, const fs::path &log) {
    ensure_dir(dest.parent_path());
    if (fs::exists(dest)) {
        std::cerr << C_YELLOW << "[info] already downloaded: " << dest << C_RESET << "\n";
        return 0;
    }
    std::string cmd = "curl -L --fail --retry 3 -o " + dest.string() + " " + url;
    return run_command_with_spinner(cmd, log);
}

// apply patch file with patch -p1
int apply_patch_file(const fs::path &patchfile, const fs::path &workdir, const fs::path &log) {
    std::string cmd = "cd " + workdir.string() + " && patch -p1 < " + patchfile.string();
    return run_command_with_spinner(cmd, log);
}

// prepare workdir: extract tarball or copy git checkout
fs::path prepare_workdir(const Recipe &r, const fs::path &SOURCES, const fs::path &WORK, const fs::path &logdir) {
    // create unique workdir
    std::string ts = now_str();
    fs::path wd = WORK / (r.name + "-" + ts);
    ensure_dir(wd);
    // If any source is a .git URL (ends with .git or contains git+), clone into sources and copy
    // Prefer first source that is a git
    for (auto &src : r.sources) {
        if (src.find(".git")!=std::string::npos) {
            fs::path gitdest = SOURCES / (r.name + "-git");
            ensure_dir(SOURCES);
            int g = git_fetch(src, gitdest, logdir / (r.name + ".git.log"));
            if (g!=0) {
                std::cerr << C_RED << "[error] git fetch failed\n" << C_RESET;
                break;
            }
            // copy working tree
            std::string cmd = "cp -a " + gitdest.string() + "/. " + wd.string();
            run_simple(cmd);
            return wd;
        }
    }
    // else look for tarballs
    for (auto &src : r.sources) {
        std::string fn = filename_from_url(src);
        fs::path dest = SOURCES / fn;
        if (!fs::exists(dest)) {
            int d = curl_download(src, dest, logdir / (r.name + ".download.log"));
            if (d!=0) {
                std::cerr << C_RED << "[error] download failed: " << src << C_RESET << "\n";
                continue;
            }
        } else {
            std::cerr << C_YELLOW << "[info] using cached " << dest << C_RESET << "\n";
        }
        // try extract
        // create a temp dir and extract
        int ex = run_simple("tar -xf " + dest.string() + " -C " + wd.string() + " --strip-components=1 2>/dev/null");
        if (ex != 0) {
            // try without strip
            run_simple("tar -xf " + dest.string() + " -C " + wd.string());
        }
        return wd;
    }
    // if no sources, maybe recipe folder: copy recipe dir
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
int apply_patches(const Recipe &r, const fs::path &workdir, const fs::path &SOURCES, const fs::path &LOGS) {
    if (r.patches.empty()) return 0;
    int acc = 0;
    for (auto &p : r.patches) {
        if (p.find(".git")!=std::string::npos) {
            // clone patches repo
            fs::path tmp = SOURCES / ("patches-" + r.name);
            acc |= git_fetch(p, tmp, LOGS / (r.name + ".patches.git.log"));
            // apply each file
            for (auto &e : fs::directory_iterator(tmp)) {
                if (!e.is_regular_file()) continue;
                acc |= apply_patch_file(e.path(), workdir, LOGS / (r.name + ".patches.log"));
            }
        } else {
            // download patch
            std::string fn = filename_from_url(p);
            fs::path dest = SOURCES / fn;
            acc |= curl_download(p, dest, LOGS / (r.name + ".patches.download.log"));
            acc |= apply_patch_file(dest, workdir, LOGS / (r.name + ".patches.log"));
        }
    }
    return acc;
}

// execute steps with DESTDIR env, logs to logfile
int execute_steps(const Recipe &r, const fs::path &workdir, const fs::path &LOGS) {
    ensure_dir(LOGS);
    fs::path logfile = LOGS / (r.name + ".build.log");
    ensure_dir(workdir);
    fs::path destdir = workdir / "DESTDIR";
    ensure_dir(destdir);
    for (auto &step : r.steps) {
        // full command: DESTDIR=... BIN=... SOURCES=... <step>
        std::string cmd = "DESTDIR=" + destdir.string() + " SOURCES=" + std::string("") + " BIN=" + std::string("") + " " + step;
        // run in workdir
        std::string full = "cd " + workdir.string() + " && " + cmd;
        int rc = run_command_with_spinner(full, logfile);
        if (rc!=0) return rc;
    }
    return 0;
}

// packaging: tar.gz the DESTDIR into package file in SOURCES or a packages dir
fs::path package_dest(const Recipe &r, const fs::path &workdir, const fs::path &PACKAGES) {
    ensure_dir(PACKAGES);
    std::string pkgname = r.name + "-" + (r.version.empty()?now_str():r.version) + ".tar.gz";
    fs::path out = PACKAGES / pkgname;
    fs::path destdir = workdir / "DESTDIR";
    // create archive
    std::string cmd = "tar -czf " + out.string() + " -C " + destdir.string() + " .";
    int rc = run_simple(cmd);
    if (rc!=0) {
        std::cerr << C_YELLOW << "[warn] packaging failed for " << r.name << C_RESET << "\n";
    }
    return out;
}

// strip binaries under DESTDIR/usr/bin and bin
int strip_binaries(const fs::path &workdir) {
    fs::path destdir = workdir / "DESTDIR";
    std::vector<fs::path> candidates;
    if (fs::exists(destdir / "usr" / "bin")) candidates.push_back(destdir / "usr" / "bin");
    if (fs::exists(destdir / "bin")) candidates.push_back(destdir / "bin");
    int acc = 0;
    for (auto &p : candidates) {
        for (auto &e : fs::recursive_directory_iterator(p)) {
            if (!e.is_regular_file()) continue;
            // attempt strip
            std::string cmd = "strip --strip-all " + e.path().string() + " 2>/dev/null || true";
            acc |= run_simple(cmd);
        }
    }
    return acc;
}

// install: default to BIN dir (safer). If INSTALL_TO_ROOT true, copy into root.
int install_from_destdir(const Recipe &r, const fs::path &workdir, const fs::path &BIN, const fs::path &LOGS, std::string &manifest_out) {
    fs::path destdir = workdir / "DESTDIR";
    if (!fs::exists(destdir)) {
        std::cerr << C_RED << "[error] no DESTDIR found for install\n" << C_RESET;
        return 1;
    }
    fs::path target = INSTALL_TO_ROOT ? fs::path("/") : BIN;
    ensure_dir(target);
    // copy files and record manifest of installed files (absolute paths)
    std::vector<std::string> manifest;
    for (auto &e : fs::recursive_directory_iterator(destdir)) {
        if (!e.is_regular_file()) continue;
        fs::path rel = fs::relative(e.path(), destdir);
        fs::path dest = target / rel;
        ensure_dir(dest.parent_path());
        // copy
        try {
            fs::copy_file(e.path(), dest, fs::copy_options::overwrite_existing);
            manifest.push_back(dest.string());
        } catch (std::exception &ex) {
            std::cerr << C_YELLOW << "[warn] copy failed: " << ex.what() << C_RESET << "\n";
        }
    }
    // save manifest to LOGS/state/<pkg>.manifest
    fs::path statedir = LOGS / "state";
    ensure_dir(statedir);
    fs::path mf = statedir / (r.name + ".manifest");
    std::ofstream ofs(mf);
    for (auto &f : manifest) ofs << f << "\n";
    ofs.close();
    manifest_out = mf.string();
    return 0;
}

// remove package by manifest
int remove_package_by_manifest(const Recipe &r, const fs::path &LOGS) {
    fs::path mf = LOGS / "state" / (r.name + ".manifest");
    if (!fs::exists(mf)) {
        std::cerr << C_YELLOW << "[info] manifest not found, nothing to remove\n" << C_RESET;
        return 0;
    }
    std::vector<std::string> files;
    {
        std::ifstream ifs(mf);
        std::string line;
        while (std::getline(ifs,line)) if (!line.empty()) files.push_back(line);
    }
    int acc = 0;
    for (auto &f : files) {
        try {
            if (fs::exists(f)) {
                fs::remove(f);
                std::cerr << C_CYAN << "[removed] " << f << C_RESET << "\n";
            }
        } catch (...) {
            std::cerr << C_YELLOW << "[warn] could not remove " << f << C_RESET << "\n";
            acc = 1;
        }
    }
    // remove manifest
    try { fs::remove(mf); } catch(...) {}
    // run post-remove hook if exists in recipe? The caller must pass and run it.
    return acc;
}

// Orchestrate single recipe: prepare, patches, build, package, install (depending on flags)
int process_recipe(const Recipe &r,
                   const fs::path &REPO, const fs::path &SOURCES,
                   const fs::path &BIN, const fs::path &LOGS,
                   bool build_only, bool do_package, bool do_install, bool strip_option) {
    std::cerr << C_MAG << "=== " << r.name << " ===" << C_RESET << "\n";
    ensure_dir(SOURCES); ensure_dir(BIN); ensure_dir(LOGS);
    fs::path workroot = fs::temp_directory_path() / "lfs-pkg-work";
    ensure_dir(workroot);
    // 1) prepare workdir
    fs::path wd = prepare_workdir(r, SOURCES, workroot, LOGS);
    std::cerr << C_CYAN << "[workdir] " << wd << C_RESET << "\n";
    // 2) apply patches
    int p = apply_patches(r, wd, SOURCES, LOGS);
    if (p!=0) { std::cerr << C_RED << "[error] patches failed\n" << C_RESET; return p; }
    // 3) build steps
    int b = execute_steps(r, wd, LOGS);
    if (b!=0) { std::cerr << C_RED << "[error] build failed\n" << C_RESET; return b; }
    // 4) optional strip
    if (strip_option || r.strip || GLOBAL_STRIP) {
        strip_binaries(wd);
    }
    // 5) package
    fs::path packages_dir = SOURCES / "packages";
    fs::path packaged;
    if (do_package) {
        packaged = package_dest(r, wd, packages_dir);
        std::cerr << C_GREEN << "[packaged] " << packaged << C_RESET << "\n";
    }
    // 6) install
    if (do_install) {
        std::string manifest_path;
        int ins = install_from_destdir(r, wd, BIN, LOGS, manifest_path);
        if (ins!=0) { std::cerr << C_RED << "[error] install failed\n" << C_RESET; return ins; }
        std::cerr << C_GREEN << "[installed] manifest: " << manifest_path << C_RESET << "\n";
    }
    return 0;
}

// high-level sync recipes git URL into REPO
int sync_repo(const fs::path &REPO, const std::string &giturl, const fs::path &LOGS) {
    if (!giturl.empty()) {
        if (fs::exists(REPO) && fs::exists(REPO/".git")) {
            std::string cmd = "git -C " + REPO.string() + " pull --rebase --autostash";
            return run_command_with_spinner(cmd, LOGS / "sync.log");
        } else {
            ensure_dir(REPO.parent_path());
            std::string cmd = "git clone --depth 1 " + giturl + " " + REPO.string();
            return run_command_with_spinner(cmd, LOGS / "sync.log");
        }
    } else {
        if (fs::exists(REPO) && fs::exists(REPO/".git")) {
            std::string cmd = "git -C " + REPO.string() + " pull --rebase --autostash";
            return run_command_with_spinner(cmd, LOGS / "sync.log");
        }
        std::cerr << C_YELLOW << "[info] no sync URL provided, not syncing\n" << C_RESET;
        return 0;
    }
}

// CLI and controller
int main(int argc, char** argv) {
    // env defaults
    fs::path REPO = fs::path(std::getenv("REPO")?std::getenv("REPO"):"./recipes");
    fs::path SOURCES = fs::path(std::getenv("SOURCES")?std::getenv("SOURCES"):"./sources");
    fs::path BIN = fs::path(std::getenv("BIN")?std::getenv("BIN"):"./bin");
    fs::path LOGS = fs::path(std::getenv("LOGS")?std::getenv("LOGS"):"./logs");
    std::string sync_url;
    // parse args
    if (argc<2) {
        std::cout << "Usage: pkg <cmd> [pkgname] [options]\nCommands: i/install, rm/remove, u/upgrade, dl/download, bp/build-only, log\nGlobal flags: --dry-run --no-reverse --sync=URL --strip --root\nEnv: REPO, SOURCES, BIN, LOGS\n";
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
        if (a=="--help"||a=="-h") {
            std::cout << "Usage: pkg <cmd> [pkg] [options]\n";
            return 0;
        }
    }
    // map abbreviations
    if (cmd=="i") cmd="install";
    else if (cmd=="rm") cmd="remove";
    else if (cmd=="u") cmd="upgrade";
    else if (cmd=="dl") cmd="download";
    else if (cmd=="bp") cmd="build-only";

    std::cerr << C_GREEN << "pkg starting. REPO="<<REPO<<" SOURCES="<<SOURCES<<" BIN="<<BIN<<" LOGS="<<LOGS<<C_RESET<<"\n";

    ensure_dir(REPO); ensure_dir(SOURCES); ensure_dir(BIN); ensure_dir(LOGS);

    // sync repo if requested
    if (!sync_url.empty()) {
        int s = sync_repo(REPO, sync_url, LOGS);
        if (s!=0) std::cerr<<C_YELLOW<<"[warn] sync returned "<<s<<C_RESET<<"\n";
    }

    // load recipes
    auto recipes = load_recipes(REPO);
    if (recipes.empty()) {
        std::cerr << C_RED << "[error] no recipes found under " << REPO << C_RESET << "\n";
        return 1;
    }
    auto idx = index_recipes(recipes);

    // find target recipes based on pkgname (if empty, operate on all)
    std::vector<Recipe> selected;
    if (pkgname.empty()) {
        // operate on all recipes (be careful)
        selected = recipes;
    } else {
        // try find matching recipe & deps
        auto it = idx.find(pkgname);
        if (it==idx.end()) {
            std::cerr << C_RED << "[error] package " << pkgname << " not found in recipes\n" << C_RESET;
            return 1;
        }
        // collect transitive closure of deps & dependents? We'll pick requested and its deps
        // For simplicity: include requested and its dependencies recursively
        std::set<int> chosen;
        std::function<void(int)> dfs = [&](int u){
            if (chosen.count(u)) return; chosen.insert(u);
            for (auto &d : recipes[u].deps) {
                if (idx.count(d)) dfs(idx[d]);
            }
        };
        dfs(it->second);
        for (int id : chosen) selected.push_back(recipes[id]);
    }

    // build global topo order among selected
    std::vector<Recipe> all_for_order = selected;
    // However, selected may be subset with arbitrary order; we need full recipes subset to topo sort
    // Make a list with only recipes that are in selected set (by name)
    std::set<std::string> selnames;
    for (auto &r: selected) selnames.insert(r.name);
    std::vector<Recipe> pool;
    for (auto &r: recipes) if (selnames.count(r.name)) pool.push_back(r);
    auto order_idx = topo_sort(pool);
    if (order_idx.empty()) return 2;
    std::vector<int> exec_order = order_idx;
    if (REVERSE_ORDER) std::reverse(exec_order.begin(), exec_order.end());

    // print plan
    std::cerr << C_CYAN << "Planned sequence:\n" << C_RESET;
    for (int id : exec_order) {
        std::cerr << " - " << pool[id].name << "\n";
    }

    // process according to command
    if (cmd=="download" || cmd=="dl") {
        // download sources & patches only
        for (int id : exec_order) {
            auto &r = pool[id];
            std::cerr << C_MAG << "[download] " << r.name << C_RESET << "\n";
            // download sources
            for (auto &surl : r.sources) {
                std::string fn = filename_from_url(surl);
                fs::path dest = SOURCES / fn;
                int rc = curl_download(surl, dest, LOGS / (r.name + ".download.log"));
                if (rc!=0) return rc;
            }
            // patches as downloads if not git
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
        std::cerr << C_GREEN << "download complete\n" << C_RESET;
        return 0;
    } else if (cmd=="build-only" || cmd=="bp") {
        for (int id : exec_order) {
            auto &r = pool[id];
            int rc = process_recipe(r, REPO, SOURCES, BIN, LOGS, true, true, false, GLOBAL_STRIP);
            if (rc!=0) return rc;
        }
        return 0;
    } else if (cmd=="install" || cmd=="i") {
        for (int id : exec_order) {
            auto &r = pool[id];
            int rc = process_recipe(r, REPO, SOURCES, BIN, LOGS, false, true, true, GLOBAL_STRIP);
            if (rc!=0) return rc;
        }
        return 0;
    } else if (cmd=="remove" || cmd=="rm") {
        // remove target package(s) in exec_order order (this is reverse install order)
        for (int id : exec_order) {
            auto &r = pool[id];
            std::cerr << C_MAG << "[remove] " << r.name << C_RESET << "\n";
            int rc = remove_package_by_manifest(r, LOGS);
            if (rc!=0) std::cerr<<C_YELLOW<<"[warn] some files couldn't be removed\n"<<C_RESET;
            // run post-remove hook if present
            if (!r.post_remove_hook.empty()) {
                std::string hookcmd = r.post_remove_hook;
                // allow substitution ${PKG}
                size_t pos;
                while ((pos = hookcmd.find("${PKG}")) != std::string::npos) hookcmd.replace(pos,6,r.name);
                run_simple(hookcmd);
            }
        }
        return 0;
    } else if (cmd=="upgrade" || cmd=="u") {
        // simplistic: re-run install for package(s)
        for (int id : exec_order) {
            auto &r = pool[id];
            std::cerr << C_MAG << "[upgrade] " << r.name << C_RESET << "\n";
            int rc = process_recipe(r, REPO, SOURCES, BIN, LOGS, false, true, true, GLOBAL_STRIP);
            if (rc!=0) return rc;
        }
        return 0;
    } else if (cmd=="log") {
        // show log for a package
        if (pkgname.empty()) {
            std::cerr << C_YELLOW << "Usage: pkg log <pkgname>\n" << C_RESET;
            return 1;
        }
        fs::path l = LOGS / (pkgname + ".build.log");
        if (!fs::exists(l)) l = LOGS / (pkgname + ".download.log");
        if (!fs::exists(l)) { std::cerr << C_YELLOW << "no log found for " << pkgname << C_RESET << "\n"; return 1; }
        std::ifstream ifs(l);
        std::string line;
        while (std::getline(ifs, line)) std::cout << line << "\n";
        return 0;
    }

    std::cerr << C_YELLOW << "[error] unknown command: " << cmd << C_RESET << "\n";
    return 1;
}
