#include "launcher_coop.h"

#include <fstream>
#include <sstream>
#include <system_error>

namespace fs = std::filesystem;

namespace pikmin {
namespace launcher {

std::string CoopFlow::clean(const std::string& text)
{
    std::string out;
    out.reserve(text.size());
    for (char c : text)
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') out.push_back(c);
    return out;
}

std::vector<std::string> CoopFlow::hostArguments(const fs::path& dir, bool vs)
{
    std::vector<std::string> args = { "--netplay-host-ice", "--netplay-code-out", (dir / "offer.txt").string(),
                                      "--netplay-answer-in", (dir / "answer.txt").string() };
    if (vs) args.insert(args.end(), { "--netplay-mode", "vs" });
    return args;
}

std::vector<std::string> CoopFlow::joinArguments(const fs::path& dir, bool vs)
{
    std::vector<std::string> args = { "--netplay-join-ice", "@" + (dir / "offer.txt").string(), "--netplay-code-out",
                                      (dir / "answer.txt").string() };
    if (vs) args.insert(args.end(), { "--netplay-mode", "vs" });
    return args;
}

bool CoopFlow::readCode(const fs::path& path, std::string& code)
{
    std::error_code ec;
    if (!fs::is_regular_file(path, ec) || fs::file_size(path, ec) > 65536) return false;
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream text;
    text << in.rdbuf();
    code = clean(text.str());
    return !code.empty();
}

// Atomic: a temporary file next to it, then a rename over it, so the game
// polling the path never reads half a code.
bool CoopFlow::writeCode(const fs::path& path, const std::string& code, std::string& error)
{
    const fs::path tmp = path.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << code << "\n";
        if (!out) {
            error = "could not write " + tmp.string();
            return false;
        }
    }
    std::error_code ec;
    fs::rename(tmp, path, ec);
    if (ec) {
        fs::remove(tmp, ec);
        error = "could not write " + path.string();
        return false;
    }
    return true;
}

void CoopFlow::reset()
{
    mRole = Role::None;
    mVs = false;
    mStage = Stage::Idle;
    mDir.clear();
    mExtra.clear();
    mOffer.clear();
    mAnswer.clear();
    mError.clear();
    mExitCode = 0;
}

bool CoopFlow::spawn(const std::vector<std::string>& arguments)
{
    std::vector<std::string> all = arguments;
    all.insert(all.end(), mExtra.begin(), mExtra.end());
    std::string error;
    if (!mSpawn(all, error)) {
        mError = error;
        mStage = Stage::Failed;
        return false;
    }
    mStage = Stage::Starting;
    return true;
}

bool CoopFlow::startHost(const fs::path& exchangeDir, std::string& error, const std::vector<std::string>& extraArguments,
                         bool vs)
{
    reset();
    mRole = Role::Host;
    mVs = vs;
    mDir = exchangeDir;
    mExtra = extraArguments;
    std::error_code ec;
    fs::create_directories(mDir, ec);
    if (ec) {
        mError = error = "could not create " + mDir.string();
        mStage = Stage::Failed;
        return false;
    }
    if (!spawn(hostArguments(mDir, mVs))) {
        error = mError;
        return false;
    }
    return true;
}

void CoopFlow::startJoin(const fs::path& exchangeDir, const std::vector<std::string>& extraArguments, bool vs)
{
    reset();
    mRole = Role::Join;
    mVs = vs;
    mDir = exchangeDir;
    mExtra = extraArguments;
    mStage = Stage::PasteOffer;
}

// The game's ICE codes (pc_netplay_ice.h): v3 "NPIX3-" + base64url of
// 0x03 and the kind byte, so an offer ('O') starts "NPIX3-A08" and an answer
// ('A') "NPIX3-A0E"; older builds print NPIX2- offers and NPIX1- answers.
static bool isOfferCode(const std::string& code)
{
    return code.rfind("NPIX3-A08", 0) == 0 || code.rfind("NPIX2-", 0) == 0;
}

static bool isAnswerCode(const std::string& code)
{
    return code.rfind("NPIX3-A0E", 0) == 0 || code.rfind("NPIX1-", 0) == 0;
}

bool CoopFlow::submitOffer(const std::string& text, std::string& error)
{
    if (mRole != Role::Join || mStage != Stage::PasteOffer) {
        error = "not waiting for an offer";
        return false;
    }
    const std::string code = clean(text);
    if (code.empty()) {
        error = "Paste the host's offer code first.";
        return false;
    }
    if (isAnswerCode(code)) {
        error = "That is an answer code. Paste the host's offer (it starts with NPIX3-A08).";
        return false;
    }
    if (!isOfferCode(code)) {
        error = "That is not an offer code (it starts with NPIX3-A08).";
        return false;
    }
    std::error_code ec;
    fs::create_directories(mDir, ec);
    if (ec || !writeCode(mDir / "offer.txt", code, error)) {
        if (error.empty()) error = "could not create " + mDir.string();
        return false;
    }
    mOffer = code;
    if (!spawn(joinArguments(mDir, mVs))) {
        error = mError;
        return false;
    }
    return true;
}

bool CoopFlow::submitAnswer(const std::string& text, std::string& error)
{
    if (mRole != Role::Host || mStage != Stage::ShowOffer) {
        error = "not waiting for an answer";
        return false;
    }
    const std::string code = clean(text);
    if (code.empty()) {
        error = "Paste the other player's answer code first.";
        return false;
    }
    if (isOfferCode(code)) {
        error = "That is your own offer. Paste the answer the other player sent back (it starts with NPIX3-A0E).";
        return false;
    }
    if (!isAnswerCode(code)) {
        error = "That is not an answer code (it starts with NPIX3-A0E).";
        return false;
    }
    if (!writeCode(mDir / "answer.txt", code, error)) return false;
    mAnswer = code;
    mStage = Stage::Connecting;
    return true;
}

void CoopFlow::poll()
{
    if (mStage == Stage::Idle || mStage == Stage::PasteOffer || mStage == Stage::Exited || mStage == Stage::Failed)
        return;
    int code = 0;
    if (!mRunning(code)) {
        mExitCode = code;
        mStage = Stage::Exited;
        return;
    }
    if (mStage != Stage::Starting) return;
    std::string text;
    if (mRole == Role::Host && readCode(mDir / "offer.txt", text)) {
        mOffer = text;
        mStage = Stage::ShowOffer;
    } else if (mRole == Role::Join && readCode(mDir / "answer.txt", text)) {
        mAnswer = text;
        mStage = Stage::Connecting;
    }
}

std::string CoopFlow::status() const
{
    switch (mStage) {
    case Stage::Idle: return std::string();
    case Stage::PasteOffer: return "Paste the offer code the host sent you, then press Join.";
    case Stage::Starting:
        return mRole == Role::Host ? "Starting the game and preparing your offer code..."
                                   : "Starting the game and preparing your answer code...";
    case Stage::ShowOffer: return "Send this offer to the other player, then paste their answer below.";
    case Stage::Connecting:
        return mRole == Role::Host ? "Connecting. The session starts in the game window once both games meet."
                                   : "Send this answer back to the host. The session starts once the host enters it.";
    case Stage::Exited:
        return mExitCode == 0 ? "The game has closed." : "The game closed with code " + std::to_string(mExitCode)
                                                             + " (see its console or netplay folder).";
    case Stage::Failed: return "Could not start the game: " + mError;
    }
    return std::string();
}

} // namespace launcher
} // namespace pikmin
