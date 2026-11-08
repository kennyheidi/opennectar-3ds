#pragma once

// Online co-op from the launcher: Host Co-op and Join Co-op, and the same
// for a 1v1 VS match (Host VS / Join VS: the same flow plus --netplay-mode vs;
// the host plays the VS map chosen in its VS menu, and its offer carries it).
//
// The launcher starts the netplay game as a child process and carries the
// connection codes through files in a private exchange folder, so the window
// can show them and take the other player's code:
//
//   Host  game --netplay-host-ice --netplay-code-out <dir>/offer.txt
//              --netplay-answer-in <dir>/answer.txt
//         The offer appears in offer.txt; the answer the player pastes is
//         written atomically to answer.txt, which the game is polling.
//   Join  the pasted offer goes to <dir>/offer.txt, then
//         game --netplay-join-ice @<dir>/offer.txt --netplay-code-out <dir>/answer.txt
//         The answer appears in answer.txt, for the player to send back.
//
// Engine-free and GUI-free (launcher_gui.cpp draws it); the process start is
// injectable so launcher_coop_test can drive the whole flow without a game.

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace pikmin {
namespace launcher {

class CoopFlow {
public:
    enum class Role { None, Host, Join };
    enum class Stage {
        Idle,
        PasteOffer,  // join: waiting for the host's offer to be pasted
        Starting,    // the game is up, its code is not written yet
        ShowOffer,   // host: offer shown, waiting for the pasted answer
        Connecting,  // host: answer handed over; join: answer shown
        Exited,      // the game has exited
        Failed,      // could not start
    };

    // Starts `game` in `dataRoot` with these arguments; returns false with a
    // reason. `running` reports whether it still runs and its exit code.
    using Spawn = std::function<bool(const std::vector<std::string>& arguments, std::string& error)>;
    using Running = std::function<bool(int& exitCode)>;

    CoopFlow(Spawn spawn, Running running) : mSpawn(std::move(spawn)), mRunning(std::move(running)) {}

    // A fresh exchange folder for this session (created; never reused).
    // `extraArguments` go after the netplay switches (for example
    // --netplay-run-root in tests).
    bool startHost(const std::filesystem::path& exchangeDir, std::string& error,
                   const std::vector<std::string>& extraArguments = {}, bool vs = false);
    void startJoin(const std::filesystem::path& exchangeDir, const std::vector<std::string>& extraArguments = {},
                   bool vs = false);
    // Join: hands over the pasted offer and starts the game. False with a
    // reason when the text is not a host offer (NPIX2-...).
    bool submitOffer(const std::string& text, std::string& error);
    // Host: hands over the pasted answer. False with a reason when the text is
    // not an answer code (NPIX1-...).
    bool submitAnswer(const std::string& text, std::string& error);
    // Once per frame: picks up the game's code and notices its exit.
    void poll();
    void reset();

    Role role() const { return mRole; }
    bool vs() const { return mVs; }
    Stage stage() const { return mStage; }
    const std::string& offer() const { return mOffer; }   // host: its offer; join: the pasted one
    const std::string& answer() const { return mAnswer; } // join: its answer; host: the pasted one
    int exitCode() const { return mExitCode; }
    const std::string& error() const { return mError; }
    const std::filesystem::path& exchangeDir() const { return mDir; }
    // A one-line status for the panel.
    std::string status() const;

    // The arguments the game gets for each role (exposed for the test).
    static std::vector<std::string> hostArguments(const std::filesystem::path& dir, bool vs = false);
    static std::vector<std::string> joinArguments(const std::filesystem::path& dir, bool vs = false);
    // Trims spaces and line breaks from a pasted code.
    static std::string clean(const std::string& text);

private:
    bool spawn(const std::vector<std::string>& arguments);
    static bool readCode(const std::filesystem::path& path, std::string& code);
    static bool writeCode(const std::filesystem::path& path, const std::string& code, std::string& error);

    Spawn mSpawn;
    Running mRunning;
    Role mRole = Role::None;
    bool mVs = false;
    Stage mStage = Stage::Idle;
    std::filesystem::path mDir;
    std::vector<std::string> mExtra;
    std::string mOffer, mAnswer, mError;
    int mExitCode = 0;
};

} // namespace launcher
} // namespace pikmin
