// Restart of the game for an online session (pc_online_menu.cpp). Kept apart
// from the game headers, which define Win32 handle types of their own.
#include "netplay/pc_online_relaunch.h"

#include <cstdio>
#include <string>
#include <vector>

#if defined(__ANDROID__)
#include <jni.h>
#include "android/pc_texpack_android.h"
#elif defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#else
#include <spawn.h>
#include <unistd.h>
extern char** environ;
#endif

#if !defined(__ANDROID__)
namespace {

// The original arguments minus any online or mode switch (with its value).
std::vector<std::string> base_args()
{
	static const char* const kWithValue[] = { "--online-host", "--online-join", "--online-save", "--online-code-out", "--online-error",
		                                      "--netplay-join-ice", "--netplay-code-out", "--netplay-answer-in", "--netplay-run-root",
		                                      "--netplay-mode" };
	static const char* const kAlone[]     = { "--online-code-clipboard", "--online-code-once", "--online-predictive",
		                                      "--netplay-host-ice", "--netplay-dialogs" };
	std::vector<std::string> raw;
#ifdef _WIN32
	int argc     = 0;
	LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
	for (int i = 1; argv != nullptr && i < argc; ++i) {
		const int n = WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, nullptr, 0, nullptr, nullptr);
		std::string s(n > 0 ? n - 1 : 0, '\0');
		if (n > 1) WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, &s[0], n, nullptr, nullptr);
		raw.push_back(s);
	}
	if (argv != nullptr) LocalFree(argv);
#else
	if (FILE* f = fopen("/proc/self/cmdline", "rb")) {
		std::string all;
		char buf[4096];
		size_t n;
		while ((n = fread(buf, 1, sizeof buf, f)) > 0) all.append(buf, n);
		fclose(f);
		size_t at = all.find('\0'); // skip argv[0]
		while (at != std::string::npos && at + 1 < all.size()) {
			const size_t end = all.find('\0', at + 1);
			raw.push_back(all.substr(at + 1, end == std::string::npos ? std::string::npos : end - at - 1));
			at = end;
		}
	}
#endif
	std::vector<std::string> out;
	for (size_t i = 0; i < raw.size(); ++i) {
		bool skip = false;
		for (const char* k : kWithValue)
			if (raw[i] == k) {
				skip = true;
				++i;
			}
		for (const char* k : kAlone) skip = skip || raw[i] == k;
		if (!skip) out.push_back(raw[i]);
	}
	return out;
}

#ifdef _WIN32
std::wstring widen(const std::string& s)
{
	const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
	std::wstring w(n > 0 ? n - 1 : 0, L'\0');
	if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
	return w;
}

// CommandLineToArgvW quoting.
void append_quoted(std::wstring& line, const std::wstring& arg)
{
	line += L" \"";
	unsigned slashes = 0;
	for (wchar_t c : arg) {
		if (c == L'\\') {
			++slashes;
			continue;
		}
		line.append(c == L'"' ? slashes * 2 + 1 : slashes, L'\\');
		slashes = 0;
		line += c;
	}
	line.append(slashes * 2, L'\\');
	line += L'"';
}
#endif

} // namespace
#endif

bool pc_online_relaunch(const std::vector<std::string>& extra, std::string* err)
{
#if defined(__ANDROID__)
	// NectarActivity restarts itself with the arguments in its intent; the
	// game's own arguments come from that intent, so only `extra` is passed.
	JavaVM* vm       = pc_android_jni_vm();
	jobject activity = pc_android_jni_activity();
	JNIEnv* env      = nullptr;
	if (vm != nullptr && vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK)
		if (vm->AttachCurrentThread(&env, nullptr) != JNI_OK) env = nullptr;
	if (env == nullptr || activity == nullptr) {
		*err = "Cannot restart the game.";
		return false;
	}
	jclass stringClass = env->FindClass("java/lang/String");
	jobjectArray array = env->NewObjectArray((jsize)extra.size(), stringClass, nullptr);
	for (size_t i = 0; i < extra.size(); ++i) {
		jstring s = env->NewStringUTF(extra[i].c_str());
		env->SetObjectArrayElement(array, (jsize)i, s);
		env->DeleteLocalRef(s);
	}
	jclass cls       = env->GetObjectClass(activity);
	jmethodID method = cls != nullptr ? env->GetMethodID(cls, "restartWithArguments", "([Ljava/lang/String;)V") : nullptr;
	if (method == nullptr) {
		env->ExceptionClear();
		*err = "Cannot restart the game.";
		return false;
	}
	env->CallVoidMethod(activity, method, array);
	env->DeleteLocalRef(array);
	env->DeleteLocalRef(cls);
	env->DeleteLocalRef(stringClass);
	printf("[online] restarting the game for the online session\n");
	fflush(stdout);
	return true;
#else
	std::vector<std::string> args = base_args();
	args.insert(args.end(), extra.begin(), extra.end());
	fflush(stdout);
#ifdef _WIN32
	wchar_t exe[MAX_PATH * 4];
	const DWORD len = GetModuleFileNameW(nullptr, exe, (DWORD)(sizeof exe / sizeof exe[0]));
	if (len == 0 || len >= sizeof exe / sizeof exe[0]) {
		*err = "Cannot find the game executable.";
		return false;
	}
	std::wstring line = L"\"" + std::wstring(exe) + L"\"";
	for (const std::string& a : args) append_quoted(line, widen(a));
	STARTUPINFOW si{};
	si.cb = sizeof si;
	PROCESS_INFORMATION pi{};
	if (!CreateProcessW(exe, &line[0], nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
		*err = "Cannot restart the game.";
		return false;
	}
	CloseHandle(pi.hThread);
	CloseHandle(pi.hProcess);
#else
	char exe[4096];
	const ssize_t len = readlink("/proc/self/exe", exe, sizeof exe - 1);
	if (len <= 0) {
		*err = "Cannot find the game executable.";
		return false;
	}
	exe[len] = '\0';
	std::vector<char*> argv;
	argv.push_back(exe);
	for (std::string& a : args) argv.push_back(&a[0]);
	argv.push_back(nullptr);
	pid_t pid = 0;
	if (posix_spawn(&pid, exe, nullptr, nullptr, argv.data(), environ) != 0) {
		*err = "Cannot restart the game.";
		return false;
	}
#endif
	printf("[online] restarting the game for the online session\n");
	fflush(stdout);
	return true;
#endif
}

void pc_online_android_toast(const char* text)
{
#if defined(__ANDROID__)
	JavaVM* vm       = pc_android_jni_vm();
	jobject activity = pc_android_jni_activity();
	JNIEnv* env      = nullptr;
	if (vm != nullptr && vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK)
		if (vm->AttachCurrentThread(&env, nullptr) != JNI_OK) env = nullptr;
	if (env == nullptr || activity == nullptr || text == nullptr) return;
	jclass cls       = env->GetObjectClass(activity);
	jmethodID method = cls != nullptr ? env->GetMethodID(cls, "showToast", "(Ljava/lang/String;)V") : nullptr;
	if (method != nullptr) {
		jstring s = env->NewStringUTF(text);
		env->CallVoidMethod(activity, method, s);
		env->DeleteLocalRef(s);
	} else {
		env->ExceptionClear();
	}
	if (cls != nullptr) env->DeleteLocalRef(cls);
#else
	(void)text;
#endif
}
