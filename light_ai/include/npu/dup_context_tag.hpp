// DupContextTag: selects the engine constructor that duplicates the context of a loaded engine,
// Engine( DupContextTag, const Engine& master, const Log& ) (RknnEngine: rknn_dup_context). SDK-free: InferencePool
// picks that constructor when its engine has one, and the host tests name the tag without rknn_api.h.

#pragma once

struct DupContextTag
{
};
