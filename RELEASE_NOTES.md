# Verified corrected source release
The supplied TXT was parsed without executing its restoration script.
Fix: fluid in-flight/censored bytes are integrated from pending arrival segments; no cancellation residue.
Included: finish_release.py, actual build/test logs, default 24h manifest/result, standard trace text and SHA256 inventory.
Production remains C++17/C++20; Python is build/test/pack only. No build trees or raw simulation inputs in the archive.
Tests cover the listed cases only. CMake is NOT_TESTED when GCC fallback is used. Sanitizers deliberately NOT_TESTED.
