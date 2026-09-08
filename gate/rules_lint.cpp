// WAIVER(R2): gate tooling; scans source text with ordinary loops.
// Enforces the checkable rules of "Rules for AI-Written Code" over src/, tests/ and shaders/.
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

struct Finding
{
    std::string rule;
    std::string file;
    int line;
    std::string message;
};

struct SourceFile
{
    fs::path path;
    std::string relative;
    std::vector<std::string> lines;
    bool interior;
    bool infrastructure;
    bool real;
    bool sim;
    bool app;
    bool tests;
    bool shader;
};

struct Function
{
    std::string name;
    std::string signature;
    std::string file;
    int line;
    int bodyStart;
    int bodyEnd;
    std::vector<std::string> body;
    bool lambda;
};

std::vector<Finding> g_findings;
std::vector<std::string> g_waivers;
std::vector<std::string> g_growthSites;
std::vector<std::string> g_featureFlags;
std::vector<std::string> g_index;

void Report(const std::string& rule, const SourceFile& f, int line, const std::string& message)
{
    g_findings.push_back(Finding{ rule, f.relative, line, message });
}

bool StartsWith(const std::string& s, const std::string& prefix)
{
    return s.rfind(prefix, 0) == 0;
}

bool Contains(const std::string& s, const std::string& needle)
{
    return s.find(needle) != std::string::npos;
}

std::string Trim(const std::string& s)
{
    const auto b = s.find_first_not_of(" \t\r\n");
    const auto e = s.find_last_not_of(" \t\r\n");
    return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
}

bool IsCommentLine(const std::string& line)
{
    const std::string t = Trim(line);
    return StartsWith(t, "//") || StartsWith(t, "/*") || StartsWith(t, "* ") || StartsWith(t, "*/") || StartsWith(t, ";");
}

std::string StripComment(const std::string& line)
{
    const auto pos = line.find("//");
    return pos == std::string::npos ? line : line.substr(0, pos);
}

std::string StripStrings(const std::string& line)
{
    std::string out;
    bool inString = false;
    char quote = 0;
    for (size_t i = 0; i < line.size(); ++i)
    {
        const char c = line[i];
        if (inString)
        {
            if (c == '\\')
                ++i;
            else if (c == quote)
                inString = false;
            continue;
        }
        if (c == '"' || c == '\'')
        {
            inString = true;
            quote = c;
            out += ' ';
            continue;
        }
        out += c;
    }
    return out;
}

bool HasWaiver(const SourceFile& f, int line, const std::string& rule)
{
    const std::string tag = "WAIVER(" + rule + ")";
    for (int i = std::max(0, line - 3); i <= std::min(static_cast<int>(f.lines.size()) - 1, line); ++i)
        if (Contains(f.lines[static_cast<size_t>(i)], tag))
            return true;
    for (int i = 0; i < std::min(12, static_cast<int>(f.lines.size())); ++i)
        if (Contains(f.lines[static_cast<size_t>(i)], tag))
            return true;
    return false;
}

void ReportUnlessWaived(const std::string& rule, const SourceFile& f, int line, const std::string& message)
{
    if (!HasWaiver(f, line, rule))
        Report(rule, f, line, message);
}

// --- file loading ------------------------------------------------------------------------------

std::vector<SourceFile> LoadSources(const fs::path& root)
{
    std::vector<SourceFile> files;
    for (const std::string dir : { "src", "tests", "shaders" })
    {
        if (!fs::exists(root / dir))
            continue;
        for (const auto& entry : fs::recursive_directory_iterator(root / dir))
        {
            if (!entry.is_regular_file())
                continue;
            const std::string ext = entry.path().extension().string();
            if (ext != ".h" && ext != ".cpp" && ext != ".hlsl" && ext != ".hlsli" && ext != ".asm")
                continue;
            SourceFile f;
            f.path = entry.path();
            f.relative = fs::relative(entry.path(), root).generic_string();
            std::ifstream in(entry.path());
            std::string line;
            while (std::getline(in, line))
                f.lines.push_back(line);
            f.interior = StartsWith(f.relative, "src/interior/");
            f.infrastructure = StartsWith(f.relative, "src/infrastructure/");
            f.real = StartsWith(f.relative, "src/effects/real/");
            f.sim = StartsWith(f.relative, "src/effects/sim/");
            f.app = StartsWith(f.relative, "src/app/");
            f.tests = StartsWith(f.relative, "tests/");
            f.shader = ext == ".hlsl" || ext == ".hlsli";
            files.push_back(f);
        }
    }
    return files;
}

// --- line-level rules -----------------------------------------------------------------------

const std::regex kLoop(R"(\b(for|while|do)\b\s*[\(\{])");
const std::regex kThrow(R"(\b(throw|try|catch|goto)\b)");
const std::regex kNew(R"((^|[^\w:])new\b)");
const std::regex kTodo(R"(TODO|FIXME|XXX\b|[Pp]laceholder|not implemented|NotImplemented)");
const std::regex kWaiver(R"(WAIVER\((R\d+)\):\s*\S)");
const std::regex kWaiverLoose(R"(WAIVER)");
const std::regex kCompound(R"((\+\+|--|\+=|-=|\*=|/=|%=|\|=|&=|\^=|<<=|>>=))");
const std::regex kThrowingStd(R"((std::get<|\.value\(\)|std::any_cast|\.at\())");
const std::regex kOsInclude(R"rx(#include\s*<(windows\.h|d3d12|dxgi|wrl/|winrt/|nvsdk_ngx|dcomp|d3d11|roapi|winstring|windows\.|shellscalingapi|inspectable|unknwn|nvOptical))rx");
const std::regex kEffectsInclude(R"(#include\s*"effects/)");
const std::regex kAppInclude(R"(#include\s*"app/)");
const std::regex kGlobal(R"(\bg_\w+)");
const std::regex kStaticLocal(R"(^\s*static\s+(?!constexpr|const\b|inline\s+constexpr))");
const std::regex kTemplateOrMacro(R"(^\s*(template\s*<|#define\b))");
const std::regex kDefaultArm(R"(^\s*default\s*:)");
const std::regex kDocComment(R"(^\s*(///|/\*\*))");
const std::regex kCommentedCode(R"(^\s*//.*(;|\{|\}|\breturn\b)\s*$)");
const std::regex kReinterpret(R"(reinterpret_cast|const_cast|\(void\s*\*\))");

void CheckLines(const SourceFile& f)
{
    int commentRun = 0;
    for (size_t i = 0; i < f.lines.size(); ++i)
    {
        const int line = static_cast<int>(i) + 1;
        const std::string& raw = f.lines[i];
        const std::string code = StripStrings(StripComment(raw));
        if (std::regex_search(raw, kWaiverLoose))
        {
            std::smatch m;
            if (!std::regex_search(raw, m, kWaiver))
                Report("R0", f, line, "waiver without rule number and reason");
            else
                g_waivers.push_back(f.relative + ":" + std::to_string(line) + " " + Trim(raw));
        }
        if (Contains(raw, "GROWTH-SITE") || Contains(code, "BoundedVector<") || Contains(code, "BoundedString<"))
            g_growthSites.push_back(f.relative + ":" + std::to_string(line) + " " + Trim(raw));
        if (Contains(raw, "FEATURE-FLAG"))
            g_featureFlags.push_back(f.relative + ":" + std::to_string(line) + " " + Trim(raw));
        if (std::regex_search(raw, kTodo) && !f.tests && !Contains(f.relative, "rules_lint"))
            Report("R19", f, line, "TODO, FIXME, placeholder or stub marker on a shipped path");
        if (f.shader || f.relative.ends_with(".asm"))
        {
            if (std::regex_search(code, kLoop))
                ReportUnlessWaived("R2", f, line, "loop statement");
            continue;
        }
        if (IsCommentLine(raw))
        {
            ++commentRun;
            if (commentRun > 2)
                Report("R28", f, line, "more than two consecutive comment lines");
            if (std::regex_search(raw, kDocComment))
                Report("R28", f, line, "doc comment");
            if (std::regex_search(raw, kCommentedCode) && !Contains(raw, "WAIVER") && !Contains(raw, "GROWTH-SITE"))
                Report("R28", f, line, "commented-out code");
        }
        else
        {
            commentRun = 0;
        }
        if (std::regex_search(code, kLoop))
            ReportUnlessWaived("R2", f, line, "loop statement (use map, filter or fold)");
        if (std::regex_search(code, kCompound))
            ReportUnlessWaived("R2", f, line, "mutation through compound assignment or increment");
        if (std::regex_search(code, kThrow))
            ReportUnlessWaived("R14", f, line, "throw, try, catch or goto");
        if (std::regex_search(code, kNew))
            ReportUnlessWaived("R14", f, line, "raw new");
        if (std::regex_search(code, kThrowingStd) && !f.tests)
            ReportUnlessWaived("R14", f, line, "throwing standard accessor (std::get, value(), at())");
        if ((f.interior || f.infrastructure || f.sim) && std::regex_search(raw, kOsInclude) && !Contains(raw, "intsafe.h"))
            ReportUnlessWaived("R10", f, line, "interior code includes an OS or vendor header");
        if ((f.interior || f.infrastructure) && (std::regex_search(raw, kEffectsInclude) || std::regex_search(raw, kAppInclude)))
            ReportUnlessWaived("R10", f, line, "interior code depends on the effect layer");
        if (!f.infrastructure && !f.tests && std::regex_search(code, kGlobal))
            ReportUnlessWaived("R11", f, line, "global or module-level mutable state");
        if (!f.infrastructure && !f.tests && std::regex_search(code, kStaticLocal) && !Contains(code, "static_assert"))
            ReportUnlessWaived("R11", f, line, "static mutable state");
        if (!f.infrastructure && !f.tests && std::regex_search(code, kTemplateOrMacro) && !Contains(code, "#define DSCREEN_"))
            ReportUnlessWaived("R31", f, line, "template or macro outside infrastructure");
        if (std::regex_search(code, kDefaultArm))
            ReportUnlessWaived("R12", f, line, "wildcard arm in a match");
        if (!f.real && !f.infrastructure && !f.tests && std::regex_search(code, kReinterpret))
            ReportUnlessWaived("R8", f, line, "type punning outside the effect layer");
    }
}

// --- function extraction ------------------------------------------------------------------------

const std::regex kFunctionHeader(
    R"(^\s*(?:\[\[nodiscard\]\]\s*)?(?:static\s+|constexpr\s+|inline\s+|friend\s+|explicit\s+|virtual\s+)*([A-Za-z_][\w:<>,\s\*&\.]*?)\s+\**&*\s*(operator\S+|[A-Za-z_]\w*(?:::[A-Za-z_]\w*)*)\s*\(([^;{}]*)\)\s*(const\s*)?(noexcept\s*)?(override\s*)?(final\s*)?(->\s*[^{]+)?\s*(\{)?\s*$)");
const std::regex kLambdaOpen(R"(\[[^\]]*\]\s*(\([^)]*\))?\s*(mutable\s*)?(noexcept\s*)?(->\s*[^{]+?)?\s*\{)");
const std::regex kControlKeyword(R"(^\s*(if|else|switch|while|for|return|do)\b)");

int BraceDelta(const std::string& code, int& opens)
{
    int delta = 0;
    opens = 0;
    for (const char c : code)
    {
        if (c == '{')
        {
            ++delta;
            ++opens;
        }
        if (c == '}')
            --delta;
    }
    return delta;
}

int ParenBalance(const std::string& code)
{
    int balance = 0;
    for (const char c : code)
        balance += c == '(' ? 1 : c == ')' ? -1 : 0;
    return balance;
}

// A signature wrapped by the formatter is joined back into one line before matching.
std::string JoinedHeader(const SourceFile& f, size_t i)
{
    std::string code = StripStrings(StripComment(f.lines[i]));
    if (ParenBalance(code) <= 0 || !Contains(code, "("))
        return code;
    std::string joined = code;
    for (size_t k = i + 1; k < f.lines.size() && k < i + 6 && ParenBalance(joined) > 0; ++k)
        joined += " " + Trim(StripStrings(StripComment(f.lines[k])));
    return ParenBalance(joined) == 0 ? joined : code;
}

std::vector<Function> ExtractFunctions(const SourceFile& f)
{
    std::vector<Function> functions;
    int depth = 0;
    for (size_t i = 0; i < f.lines.size(); ++i)
    {
        const std::string code = JoinedHeader(f, i);
        std::smatch m;
        const bool header = std::regex_match(code, m, kFunctionHeader) && !std::regex_search(code, kControlKeyword);
        const bool lambda = std::regex_search(code, kLambdaOpen);
        if (header && depth >= 0)
        {
            std::string returnType = Trim(m[1].str());
            if (returnType == "return" || returnType == "else" || Contains(returnType, "("))
            {
                int o = 0;
                depth += BraceDelta(code, o);
                continue;
            }
            Function fn;
            fn.name = m[2].str();
            fn.signature = Trim(code);
            fn.file = f.relative;
            fn.line = static_cast<int>(i) + 1;
            fn.lambda = false;
            size_t j = i;
            int local = 0;
            bool started = false;
            for (; j < f.lines.size(); ++j)
            {
                const std::string c2 = StripStrings(StripComment(f.lines[j]));
                if (!started && Contains(c2, ";") && !Contains(c2, "{"))
                    break;
                int o = 0;
                const int d = BraceDelta(c2, o);
                if (!started && o > 0)
                {
                    started = true;
                    fn.bodyStart = static_cast<int>(j) + 1;
                }
                if (started)
                    fn.body.push_back(c2);
                local += d;
                if (started && local <= 0)
                {
                    fn.bodyEnd = static_cast<int>(j) + 1;
                    break;
                }
            }
            if (started)
            {
                functions.push_back(fn);
                i = j;
            }
            continue;
        }
        if (lambda && !header)
        {
            Function fn;
            fn.name = "lambda";
            fn.signature = Trim(code);
            fn.file = f.relative;
            fn.line = static_cast<int>(i) + 1;
            fn.lambda = true;
            fn.bodyStart = fn.line;
            int local = 0;
            size_t j = i;
            for (; j < f.lines.size(); ++j)
            {
                const std::string c2 = StripStrings(StripComment(f.lines[j]));
                fn.body.push_back(c2);
                int o = 0;
                local += BraceDelta(c2, o);
                if (local <= 0)
                    break;
            }
            fn.bodyEnd = static_cast<int>(j) + 1;
            if (fn.body.size() > 1)
                functions.push_back(fn);
        }
        int o = 0;
        depth += BraceDelta(code, o);
    }
    return functions;
}

// --- function-level rules ------------------------------------------------------------------------

struct BodyStats
{
    int statements;
    int decisions;
    int nestedConditionals;
    bool compoundCondition;
    bool passThrough;
    std::vector<std::string> tokens;
};

const std::regex kIf(R"(\bif\s*\()");
const std::regex kTernary(R"(\?)");
const std::regex kLogic(R"(&&|\|\|)");
const std::regex kSwitch(R"(\bswitch\s*\()");
const std::regex kIfCondition(R"(\bif\s*\((.*)\)\s*$)");
const std::regex kToken(R"([A-Za-z_]\w*|\d+(\.\d+)?f?|::|->|[^\s\w])");
const std::regex kInlineLambda(R"(\[[^\]]*\]\s*(\([^)]*\))?\s*(mutable\s*)?(noexcept\s*)?(->\s*[^{]+?)?\s*\{[^{}]*\})");
const std::regex kLambdaHeaderOnly(R"(\[[^\]]*\]\s*\([^)]*\)\s*(noexcept\s*)?(->\s*[^{;]+)?\s*$)");

std::string WithoutInlineLambdas(const std::string& line)
{
    std::string out = line;
    for (int guard = 0; guard < 8 && std::regex_search(out, kInlineLambda); ++guard)
        out = std::regex_replace(out, kInlineLambda, "LAMBDA", std::regex_constants::format_first_only);
    return out;
}

std::vector<std::string> Tokens(const std::string& code)
{
    std::vector<std::string> out;
    for (auto it = std::sregex_iterator(code.begin(), code.end(), kToken); it != std::sregex_iterator(); ++it)
    {
        const std::string t = it->str();
        const auto next = std::next(it);
        const bool called = next != std::sregex_iterator() && (next->str() == "(" || next->str() == "::" || next->str() == "<");
        const bool qualified = out.size() > 0 && out.back() == "::";
        if (std::isalpha(static_cast<unsigned char>(t[0])) || t[0] == '_')
            out.push_back(called || qualified || t == "return" || t == "if" || t == "else" || t == "const" || t == "auto" ? t : "ID");
        else
            out.push_back(t);
    }
    return out;
}

BodyStats Analyse(const Function& fn)
{
    BodyStats s{ 0, 0, 0, false, false, {} };
    int depth = 0;
    int lambdaDepth = -1;
    int switchDepth = -1;
    int ifDepth = -1;
    bool sawIfBlock = false;
    bool pendingLambda = false;
    bool switchOpened = false;
    for (size_t i = 0; i < fn.body.size(); ++i)
    {
        const std::string line = WithoutInlineLambdas(fn.body[i]);
        const bool inLambda = lambdaDepth >= 0;
        const bool inSwitch = switchDepth >= 0;
        if (!inLambda && !inSwitch)
        {
            const std::string forTokens = line;
            const auto t = Tokens(forTokens);
            s.tokens.insert(s.tokens.end(), t.begin(), t.end());
            if (std::regex_search(line, kIf))
            {
                ++s.decisions;
                if (ifDepth >= 0 && depth > ifDepth)
                    ++s.nestedConditionals;
                std::smatch cm;
                if (std::regex_search(line, cm, kIfCondition) && std::regex_search(cm[1].str(), kLogic))
                    s.compoundCondition = true;
                if (ifDepth < 0)
                    ifDepth = depth;
                sawIfBlock = true;
            }
            for (auto it = std::sregex_iterator(line.begin(), line.end(), kLogic); it != std::sregex_iterator(); ++it)
                ++s.decisions;
            if (std::regex_search(line, kTernary))
                ++s.decisions;
            if (std::regex_search(line, kSwitch))
            {
                ++s.decisions;
                switchDepth = depth;
            }
            for (const char c : line)
                if (c == ';')
                    ++s.statements;
        }
        const bool lambdaHere = (std::regex_search(line, kLambdaOpen) || (pendingLambda && Contains(line, "{"))) && i > 0;
        pendingLambda = std::regex_search(line, kLambdaHeaderOnly);
        int o = 0;
        const int d = BraceDelta(line, o);
        if (lambdaHere && lambdaDepth < 0)
            lambdaDepth = depth;
        depth += d;
        if (lambdaDepth >= 0 && depth <= lambdaDepth)
            lambdaDepth = -1;
        if (switchDepth >= 0 && depth > switchDepth)
            switchOpened = true;
        if (switchDepth >= 0 && switchOpened && depth <= switchDepth)
        {
            switchDepth = -1;
            switchOpened = false;
        }
        if (ifDepth >= 0 && depth <= ifDepth && sawIfBlock && i > 0 && !std::regex_search(line, kIf))
            ifDepth = -1;
    }
    if (fn.body.size() >= 3 && fn.body.size() <= 4)
    {
        const std::string only = Trim(fn.body[1]);
        std::smatch pm;
        static const std::regex pass(R"(^return\s+([A-Za-z_][\w:]*)\((.*)\);$)");
        std::smatch hm;
        std::string params;
        if (std::regex_match(only, pm, pass))
        {
            const std::regex sig(R"(\(([^)]*)\))");
            if (std::regex_search(fn.signature, hm, sig))
            {
                std::string names;
                std::stringstream ss(hm[1].str());
                std::string part;
                while (std::getline(ss, part, ','))
                {
                    const auto p = Trim(part);
                    const auto space = p.find_last_of(" &*");
                    names += (names.empty() ? "" : ", ") + (space == std::string::npos ? p : p.substr(space + 1));
                }
                const bool constructs = Contains(fn.signature, pm[1].str() + " ") || Contains(fn.signature, pm[1].str() + ",") || Contains(fn.signature, pm[1].str() + ">");
                s.passThrough = !names.empty() && Trim(pm[2].str()) == names && pm[1].str() != fn.name && !constructs;
            }
        }
    }
    return s;
}

void CheckFunctions(const SourceFile& f, const std::vector<Function>& functions)
{
    if (f.tests || Contains(f.relative, "rules_lint"))
        return;
    for (const Function& fn : functions)
    {
        const BodyStats s = Analyse(fn);
        if (s.statements > 5)
            ReportUnlessWaived("R1", f, fn.line, fn.name + ": " + std::to_string(s.statements) + " statements (max 5)");
        if (s.decisions > 1)
            ReportUnlessWaived("R1", f, fn.line, fn.name + ": cyclomatic complexity " + std::to_string(s.decisions + 1) + " (max 2)");
        if (s.nestedConditionals > 0)
            ReportUnlessWaived("R1", f, fn.line, fn.name + ": conditional inside a conditional");
        if (s.compoundCondition)
            ReportUnlessWaived("R1", f, fn.line, fn.name + ": inline compound condition (name the predicate)");
        if (s.passThrough)
            ReportUnlessWaived("R5", f, fn.line, fn.name + ": pass-through function");
        if (!fn.lambda)
            g_index.push_back(f.relative + ":" + std::to_string(fn.line) + " " + fn.signature);
    }
}

// --- must-use, reachability, clones --------------------------------------------------------------

const std::regex kDeclaration(
    R"(^\s*(?:static\s+|constexpr\s+|inline\s+|friend\s+|explicit\s+|virtual\s+)*([A-Za-z_][\w:<>,\s\*&]*?)\s+\**&*\s*([A-Za-z_]\w*(?:::[A-Za-z_]\w*)*)\s*\([^;{}]*\)\s*(?:const\s*)?(?:noexcept\s*)?(?:override\s*)?(?:final\s*)?(?:=\s*default\s*)?(?:=\s*0\s*)?[;{]?\s*$)");

std::set<std::string> g_nodiscardNames;

void CollectNodiscard(const SourceFile& f)
{
    static const std::regex named(R"(\[\[nodiscard\]\][^(]*?([A-Za-z_]\w*)\s*\()");
    for (size_t i = 0; i + 1 < f.lines.size(); ++i)
    {
        const std::string joined = f.lines[i] + " " + f.lines[i + 1];
        for (auto it = std::sregex_iterator(joined.begin(), joined.end(), named); it != std::sregex_iterator(); ++it)
            g_nodiscardNames.insert((*it)[1].str());
    }
}

void CheckMustUse(const SourceFile& f)
{
    if (f.tests || f.shader || Contains(f.relative, "rules_lint") || f.relative.ends_with(".asm"))
        return;
    int depth = 0;
    for (size_t i = 0; i < f.lines.size(); ++i)
    {
        const std::string code = StripStrings(StripComment(f.lines[i]));
        std::smatch m;
        const bool atScope = depth <= 2;
        if (atScope && std::regex_match(code, m, kDeclaration) && !std::regex_search(code, kControlKeyword))
        {
            const std::string type = Trim(m[1].str());
            const std::string qualified = m[2].str();
            const std::string name = qualified.rfind("::") == std::string::npos ? qualified : qualified.substr(qualified.rfind("::") + 2);
            const bool isVoid = type == "void" || StartsWith(type, "void ") || type.ends_with(" void");
            const bool keywordType = type == "constexpr" || type == "static" || type == "inline" || type == "explicit" || type == "friend";
            const bool ctorLike = keywordType || Contains(type, "using") || type.empty() || name == "main" || name == "wmain" || StartsWith(name, "operator") || g_nodiscardNames.count(name) > 0;
            const bool previous = i > 0 && Contains(f.lines[i - 1], "[[nodiscard]]");
            const bool here = Contains(f.lines[i], "[[nodiscard]]");
            if (!isVoid && !ctorLike && !previous && !here && !Contains(code, "=") && !Contains(type, "explicit"))
                ReportUnlessWaived("R17", f, static_cast<int>(i) + 1, name + ": non-void function without [[nodiscard]]");
        }
        int o = 0;
        depth += BraceDelta(code, o);
    }
}

bool IsIdentifierChar(char c)
{
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}

// Identifiers followed by one of ( ; , ) < count as references (calls, pointers passed on); member accesses
// (a.f, p->f) do not name our functions. callsOnly keeps the names followed by ( for the call graph.
std::vector<std::string> ReferencedNames(const std::string& line, bool callsOnly)
{
    std::vector<std::string> names;
    size_t i = 0;
    while (i < line.size())
    {
        if (!IsIdentifierChar(line[i]) || std::isdigit(static_cast<unsigned char>(line[i])) != 0)
        {
            ++i;
            continue;
        }
        const size_t start = i;
        while (i < line.size() && IsIdentifierChar(line[i]))
            ++i;
        size_t next = i;
        while (next < line.size() && (line[next] == ' ' || line[next] == '\t'))
            ++next;
        const bool member = start >= 1 && (line[start - 1] == '.' || (start >= 2 && line[start - 2] == '-' && line[start - 1] == '>'));
        const bool call = next < line.size() && line[next] == '(';
        const bool reference = call || (next < line.size() && (line[next] == ';' || line[next] == ',' || line[next] == ')' || line[next] == '<'));
        if (!member && (callsOnly ? call : reference))
            names.push_back(line.substr(start, i - start));
    }
    return names;
}

void CheckReachabilityAndClones(const std::vector<SourceFile>& files, const std::vector<Function>& all)
{
    std::map<std::string, int> references;
    for (const SourceFile& f : files)
        for (const std::string& l : f.lines)
            for (const std::string& name : ReferencedNames(StripComment(l), false))
                ++references[name];
    const std::set<std::string> entryPoints{ "main",
                                             "wmain",
                                             "WindowProc",
                                             "TraceEnter",
                                             "TraceExit",
                                             "__cyg_profile_func_enter",
                                             "__cyg_profile_func_exit",
                                             "ContractViolation",
                                             "DumpTrace",
                                             "Contract",
                                             "Parse",
                                             "operator==",
                                             "operator<=>",
                                             "Get",
                                             "Size",
                                             "IsEmpty",
                                             "IsFull",
                                             "At",
                                             "Items",
                                             "Push",
                                             "Last",
                                             "HasIndex",
                                             "CString",
                                             "Capacity",
                                             "Describe",
                                             "Analyse" };
    std::map<std::string, const Function*> bodies;
    for (const Function& fn : all)
    {
        if (fn.lambda)
            continue;
        std::string bare = fn.name;
        const auto pos = bare.rfind("::");
        if (pos != std::string::npos)
            bare = bare.substr(pos + 2);
        if (entryPoints.count(bare) || StartsWith(bare, "operator") || StartsWith(fn.file, "tests/") || Contains(fn.file, "rules_lint"))
            continue;
        static const std::regex identifier(R"(^[A-Za-z_]\w*$)");
        if (!std::regex_match(bare, identifier))
            continue;
        const auto counted = references.find(bare);
        const int uses = counted == references.end() ? 0 : counted->second;
        if (uses <= 1)
            g_findings.push_back(Finding{ "R6", fn.file, fn.line, bare + ": no reference outside its definition" });
        const BodyStats s = Analyse(fn);
        if (s.tokens.size() >= 30)
        {
            std::string key;
            for (const std::string& t : s.tokens)
                key += t + " ";
            const auto existing = bodies.find(key);
            const auto source = std::find_if(files.begin(), files.end(), [&fn](const SourceFile& f) { return f.relative == fn.file; });
            if (existing != bodies.end() && source != files.end())
                ReportUnlessWaived("R7", *source, fn.line, bare + ": body duplicates " + existing->second->name + " at " + existing->second->file + ":" + std::to_string(existing->second->line));
            else
                bodies.emplace(key, &fn);
        }
    }
}

// --- recursion (R3): the static call graph over uniquely named functions must be acyclic -----------

std::string BareName(const std::string& name)
{
    const auto pos = name.rfind("::");
    return pos == std::string::npos ? name : name.substr(pos + 2);
}

bool WalksIntoCycle(const std::string& node, const std::map<std::string, std::set<std::string>>& edges, std::vector<std::string>& path, std::set<std::string>& done, std::string& cycle)
{
    if (done.count(node))
        return false;
    const auto seen = std::find(path.begin(), path.end(), node);
    if (seen != path.end())
    {
        cycle = node;
        for (auto it = seen; it != path.end(); ++it)
            cycle += " -> " + *it;
        return true;
    }
    path.push_back(node);
    const auto found = edges.find(node);
    if (found != edges.end())
        for (const std::string& next : found->second)
            if (WalksIntoCycle(next, edges, path, done, cycle))
                return true;
    path.pop_back();
    done.insert(node);
    return false;
}

void CheckRecursion(const std::vector<SourceFile>& files, const std::vector<Function>& all)
{
    std::map<std::string, int> definitions;
    for (const Function& fn : all)
        if (!fn.lambda && !StartsWith(fn.file, "tests/") && !Contains(fn.file, "rules_lint"))
            ++definitions[BareName(fn.name)];
    std::map<std::string, std::set<std::string>> edges;
    std::map<std::string, const Function*> owners;
    for (const Function& fn : all)
    {
        const std::string bare = BareName(fn.name);
        if (fn.lambda || definitions[bare] != 1)
            continue;
        owners[bare] = &fn;
        for (const std::string& line : fn.body)
            for (const std::string& callee : ReferencedNames(line, true))
                if (definitions[callee] == 1)
                    edges[bare].insert(callee);
    }
    std::set<std::string> done;
    std::set<std::string> reported;
    for (const auto& [name, fn] : owners)
    {
        std::vector<std::string> path;
        std::string cycle;
        if (WalksIntoCycle(name, edges, path, done, cycle) && reported.insert(cycle.substr(0, cycle.find(' '))).second)
        {
            const Function* culprit = owners[cycle.substr(0, cycle.find(' '))];
            const auto source = std::find_if(files.begin(), files.end(), [culprit](const SourceFile& f) { return f.relative == culprit->file; });
            if (source != files.end())
                ReportUnlessWaived("R3", *source, culprit->line, "recursion through the static call graph: " + cycle);
        }
    }
}

void WriteList(const fs::path& path, const std::vector<std::string>& lines)
{
    std::ofstream out(path);
    for (const std::string& l : lines)
        out << l << "\n";
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        std::fprintf(stderr, "usage: rules_lint <repo root> <output dir>\n");
        return 2;
    }
    const fs::path root(argv[1]);
    const fs::path out(argv[2]);
    fs::create_directories(out);
    const std::vector<SourceFile> files = LoadSources(root);
    std::vector<Function> all;
    for (const SourceFile& f : files)
        CollectNodiscard(f);
    for (const SourceFile& f : files)
    {
        CheckLines(f);
        if (f.shader || f.relative.ends_with(".asm"))
            continue;
        const std::vector<Function> fns = ExtractFunctions(f);
        CheckFunctions(f, fns);
        CheckMustUse(f);
        all.insert(all.end(), fns.begin(), fns.end());
    }
    CheckReachabilityAndClones(files, all);
    CheckRecursion(files, all);
    std::sort(g_index.begin(), g_index.end());
    WriteList(out / "function_index.txt", g_index);
    WriteList(out / "waivers.txt", g_waivers);
    WriteList(out / "growth_sites.txt", g_growthSites);
    WriteList(out / "feature_flags.txt", g_featureFlags);
    std::sort(g_findings.begin(), g_findings.end(), [](const Finding& a, const Finding& b) { return a.file == b.file ? a.line < b.line : a.file < b.file; });
    for (const Finding& f : g_findings)
        std::printf("%s:%d: [%s] %s\n", f.file.c_str(), f.line, f.rule.c_str(), f.message.c_str());
    std::printf("rules_lint: %zu finding(s), %zu waiver(s), %zu growth site(s), %zu feature flag(s), %zu indexed function(s)\n", g_findings.size(), g_waivers.size(), g_growthSites.size(),
                g_featureFlags.size(), g_index.size());
    return g_findings.empty() ? 0 : 1;
}
