#ifndef AUDIO_OUTPUT_MANAGER_H
#define AUDIO_OUTPUT_MANAGER_H

#include "Config.h"
#include <MediaNode.h>
#include <MediaRoster.h>
#include <String.h>
#include <vector>

#if ENABLE_LOCAL_OUTPUT

class NodeReleaser {
public:
    explicit NodeReleaser(const media_node& n) : fNode(n), fArmed(true) {}
    ~NodeReleaser() {
        if (fArmed) {
            BMediaRoster* roster = BMediaRoster::Roster();
            if (roster)
                roster->ReleaseNode(fNode);
        }
    }
    void Dismiss() { fArmed = false; }
private:
    media_node fNode;
    bool       fArmed;
};

struct OutputBusInfo {
    BString    deviceName;     // e.g. "HD Audio", "USB Audio (RMX2)"
    BString    busName;        // e.g. "output 0 (ch 1-2)"
    media_node deviceNode;     // the physical output node
    media_input input;         // the specific bus's input
    bool        isSystemDefaultDevice; // mixer currently on this device?
};

enum class OutputTarget {
    SystemDefault,
    DirectShared,
    DirectExclusive
};

enum class MixerConflictPolicy {
    Disconnect,
    SwitchToDevice
};

class AudioOutputManager {
public:
    AudioOutputManager();
    ~AudioOutputManager();

    status_t Enumerate(std::vector<OutputBusInfo>& out);
    status_t CurrentSystemDefault(media_node& node, BString& deviceName);

    status_t Acquire(const OutputBusInfo& target,
                     OutputTarget mode,
                     MixerConflictPolicy policy,
                     const BString& fallbackDeviceName,
                     media_node& outNode,
                     media_input& outInput);

    void Release();
    bool RecoverFromBreadcrumb();

private:
    bool fAlteredSystem;
    MixerConflictPolicy fAlteredPolicy;
    
    // For Disconnect policy restore: every mixer->device connection we broke
    // (one per bus; Shared breaks the target bus, Exclusive breaks them all).
    std::vector<media_output> fSavedMixerOutputs;
    
    // For SwitchToDevice policy restore
    media_input fOriginalSystemInput;
    bool fHasOriginalSystemInput;
    
    media_node fAcquiredNode;
    bool fHasAcquiredNode;

    status_t _WriteBreadcrumb(const BString& originalDeviceName, const BString& originalInputName, MixerConflictPolicy policy);
    void _ClearBreadcrumb();
    BString _GetBreadcrumbPath();
};

#endif // ENABLE_LOCAL_OUTPUT

#endif // AUDIO_OUTPUT_MANAGER_H
