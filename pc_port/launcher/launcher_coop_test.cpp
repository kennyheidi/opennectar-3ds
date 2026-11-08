// Host-run test for the launcher's Host Co-op / Join Co-op flow
// (launcher_coop.h): the arguments each role gives the game, the code files
// in the exchange folder, the paste checks and the stages, with a fake game
// that writes its code the way the netplay game does.

#include "launcher_coop.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using pikmin::launcher::CoopFlow;

namespace {

int sFailures = 0;

void check(bool ok, const char* what)
{
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++sFailures;
    }
}

std::string readAll(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}

void writeAll(const fs::path& p, const std::string& text)
{
    std::ofstream out(p, std::ios::binary);
    out << text;
}

// The value after `flag` in a game command line.
std::string argAfter(const std::vector<std::string>& args, const char* flag)
{
    for (size_t i = 0; i + 1 < args.size(); ++i)
        if (args[i] == flag) return args[i + 1];
    return std::string();
}

struct FakeGame {
    std::vector<std::string> args;
    bool running = false;
    int exitCode = 0;
    int spawns = 0;
    bool failSpawn = false;
};

} // namespace

int main()
{
    const fs::path root = fs::temp_directory_path() / "launcher_coop_test";
    std::error_code ec;
    fs::remove_all(root, ec);

    FakeGame game;
    CoopFlow flow(
        [&](const std::vector<std::string>& args, std::string& error) {
            if (game.failSpawn) {
                error = "no such file";
                return false;
            }
            game.args = args;
            game.running = true;
            ++game.spawns;
            return true;
        },
        [&](int& code) {
            code = game.exitCode;
            return game.running;
        });

    // Host.
    {
        std::string err;
        const fs::path dir = root / "host";
        check(flow.startHost(dir, err, { "--netplay-run-root", (dir / "runs").string() }), "host starts");
        check(flow.role() == CoopFlow::Role::Host && flow.stage() == CoopFlow::Stage::Starting, "host is starting");
        check(game.args.size() == 7 && game.args[0] == "--netplay-host-ice", "host switch first");
        check(argAfter(game.args, "--netplay-code-out") == (dir / "offer.txt").string(), "host writes its offer");
        check(argAfter(game.args, "--netplay-answer-in") == (dir / "answer.txt").string(), "host polls the answer");
        check(argAfter(game.args, "--netplay-run-root") == (dir / "runs").string(), "extra arguments passed");
        check(fs::is_directory(dir), "exchange folder created");
        flow.poll();
        check(flow.stage() == CoopFlow::Stage::Starting, "still starting without an offer file");
        check(!flow.submitAnswer("NPIX1-abc", err), "no answer before the offer is shown");
        writeAll(dir / "offer.txt", "NPIX2-OFFERCODE\n");
        flow.poll();
        check(flow.stage() == CoopFlow::Stage::ShowOffer && flow.offer() == "NPIX2-OFFERCODE", "offer shown");
        check(!flow.submitAnswer("", err) && !err.empty(), "empty answer refused");
        check(!flow.submitAnswer("NPIX2-OFFERCODE", err) && err.find("own offer") != std::string::npos,
              "own offer refused as an answer");
        check(!flow.submitAnswer("hello", err), "garbage answer refused");
        check(!fs::exists(dir / "answer.txt"), "no answer file written for a refused paste");
        check(flow.submitAnswer("  NPIX1-ANS\r\nWER  ", err), "answer accepted");
        check(readAll(dir / "answer.txt") == "NPIX1-ANSWER\n", "answer file holds the cleaned code");
        check(!fs::exists(dir / "answer.txt.tmp"), "no temporary file left behind");
        check(flow.stage() == CoopFlow::Stage::Connecting, "host connecting");
        game.running = false;
        game.exitCode = 0;
        flow.poll();
        check(flow.stage() == CoopFlow::Stage::Exited && flow.exitCode() == 0, "host exit noticed");
    }

    // Join.
    {
        std::string err;
        const fs::path dir = root / "join";
        game = FakeGame();
        flow.startJoin(dir);
        check(flow.stage() == CoopFlow::Stage::PasteOffer && game.spawns == 0, "join waits for the offer, no game yet");
        check(!flow.submitOffer("NPIX1-ANSWER", err) && err.find("answer code") != std::string::npos,
              "an answer pasted as the offer is refused");
        check(!flow.submitOffer("   ", err), "empty offer refused");
        check(game.spawns == 0, "no game for refused pastes");
        check(flow.submitOffer("NPIX2-OFFER\nCODE", err), "offer accepted");
        check(readAll(dir / "offer.txt") == "NPIX2-OFFERCODE\n", "offer file holds the cleaned code");
        check(game.spawns == 1 && game.args[0] == "--netplay-join-ice", "joiner started");
        check(game.args[1] == "@" + (dir / "offer.txt").string(), "joiner reads the offer file");
        check(argAfter(game.args, "--netplay-code-out") == (dir / "answer.txt").string(), "joiner writes its answer");
        flow.poll();
        check(flow.stage() == CoopFlow::Stage::Starting, "joiner starting");
        writeAll(dir / "answer.txt", "NPIX1-MYANSWER\n");
        flow.poll();
        check(flow.stage() == CoopFlow::Stage::Connecting && flow.answer() == "NPIX1-MYANSWER", "answer shown");
        game.running = false;
        game.exitCode = 5;
        flow.poll();
        check(flow.stage() == CoopFlow::Stage::Exited && flow.exitCode() == 5
                  && flow.status().find("code 5") != std::string::npos,
              "joiner exit code reported");
    }

    // Host VS / Join VS: the same flow, with --netplay-mode vs.
    {
        std::string err;
        game = FakeGame();
        const fs::path dir = root / "vs-host";
        check(flow.startHost(dir, err, {}, true) && flow.vs(), "VS host starts");
        check(game.args.size() == 7 && argAfter(game.args, "--netplay-mode") == "vs", "VS host asks for VS");
        check(CoopFlow::hostArguments(dir) == std::vector<std::string>(game.args.begin(), game.args.begin() + 5),
              "VS host keeps the co-op arguments");
        game = FakeGame();
        flow.startJoin(root / "vs-join", {}, true);
        check(flow.vs() && flow.submitOffer("NPIX2-OFFER", err), "VS joiner takes the offer");
        check(game.args[0] == "--netplay-join-ice" && argAfter(game.args, "--netplay-mode") == "vs", "VS joiner asks for VS");
        flow.startJoin(root / "coop-join");
        check(!flow.vs(), "a co-op join after a VS one is co-op");
        check(argAfter(CoopFlow::joinArguments(root), "--netplay-mode").empty(), "co-op arguments name no mode");
    }

    // A game that cannot start.
    {
        std::string err;
        game = FakeGame();
        game.failSpawn = true;
        check(!flow.startHost(root / "fail", err) && flow.stage() == CoopFlow::Stage::Failed
                  && flow.status().find("no such file") != std::string::npos,
              "spawn failure reported");
    }

    fs::remove_all(root, ec);
    if (sFailures == 0) std::printf("launcher_coop_test: PASS\n");
    else std::printf("launcher_coop_test: %d FAILURES\n", sFailures);
    return sFailures == 0 ? 0 : 1;
}
