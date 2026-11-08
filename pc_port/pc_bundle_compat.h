#pragma once

#include <cstring>

// The retail bundle contains this one misspelled animation key. Canonicalize
// it in-place when the bundle is loaded; both strings intentionally have the
// same size so the bundle reader's String storage never needs to grow.
inline bool pcCanonicalizeBundleAnimationPath(char* path)
{
	static constexpr char badLogoPath[]  = "cinemas/titles/logoMotioe/logo.dck";
	static constexpr char goodLogoPath[] = "cinemas/titles/logoMotion/logo.dck";
	static_assert(sizeof(badLogoPath) == sizeof(goodLogoPath), "logo path alias must be in-place");
	if (!path || std::strcmp(path, badLogoPath) != 0) {
		return false;
	}
	std::memcpy(path, goodLogoPath, sizeof(goodLogoPath));
	return true;
}
