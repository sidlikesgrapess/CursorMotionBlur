// Appended to a test copy of the mod: stands in for explorer.exe and runs the mod's own band window thread (the copy
// falls back to a plain topmost window, as only explorer.exe may create one in a higher band). Runs until killed.

int wmain() {
    HANDLE t = CreateThread(nullptr, 0, BandThread, nullptr, 0, nullptr);
    if (!t) return 1;
    WaitForSingleObject(t, INFINITE);
    return 0;
}
