## camera reconnect: remove mutable dev_ (refactor)

Current reconnect() re-assigns dev_ (member made non-const) to adopt a
re-enumerated node after a USB replug. This defeats the class invariant
(one instance = one immutable node). Refactor plan, to apply after the
current closed revision.

CONSTRAINT: dynamic memory allocation is forbidden in this project —
no new/delete, no std::unique_ptr/shared_ptr (the temporary mutable
dev_ is equally off-limits as the final shape).

Plan (no heap, no object replacement):
1. Move node scanning (numeric /dev/video* + QUERYCAP usb filter) into
   a free function, e.g. `find_usb_capture_node(width, height)`
   returning a std::string.
2. Parameterize CameraV4L2: dev path leaves the class — open/setup
   methods take the node path as an argument; the fd/buffer state
   stays in the instance, teardown() resets it. The member dev_ is
   removed entirely (not restored as const).
3. Pipeline keeps ONE CameraV4L2 member and stores the current node in
   its own `std::string cam_node_` (set from ctor args). Recovery path:
   teardown() -> scan -> cam_node_ = found -> camera.open(cam_node_).
   Mutable identity moves to the pipeline, the camera class stays
   invariant-clean and allocation-free.

Trigger for this entry: verification round with USB replug recovery.

Status: implemented and verified 2026-10-25 (replug recovery on attempt 13 with a node change video20 -> video21; permanent unplug gives up, exit 1).
