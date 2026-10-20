# What happens in a draw call?

The `ngl_draw()` call is split in 4 passes:

1. Visit the graph
2. Honor node resources (prefetch and release)
3. Update the nodes
4. Actual GL drawing


## Visit

The first pass performed by `ngli_node_honor_release_prefetch()` is to visit
the scene.

By default, `ngli_node_visit()` will crawl every node parameter to find the
child nodes. It is meant to flag every node as *active* or *inactive* for a
given time. The final state of a node depends on the knowledge of the whole
graph because of diamond shaped tree. A typical case is an active branch and an
inactive branch sharing a common node; that leaf node must stay active even
though one its parent is inactive.

The visit also determines the timestamp each node should prepare for. Time
filters pass down the times when their children can be updated, including any
fixed `render_time`. For a node shared by several branches, the branch that
needs it soonest determines the timestamp. This uses the delay until the
update can happen, since a fixed `render_time` can request a frame from a
different point in time.

**Note**: if a branch is already inactive, the visitor is unlikely to follow it
down, preventing a huge overhead on the tree with a large number of time range
filtered branches.


## Prefetch/Release

The second pass performed by `ngli_node_honor_release_prefetch()` will crawl
every node previously visited by the first pass and execute a prefetch or a
release according to how it's been flagged. Previously unvisited nodes (and
their children) are ignored.

For video, prefetching opens the file and starts decoding frames in advance.
The player seeks to the first frame the scene is expected to need before
starting the decoder. The timestamp chosen during the visit is mapped through
`Media.time_anim` to find the seek target.

For example, with no time remapping, a video visible during scene time
`[3s, 5s)` can be prefetched when drawing at `2.5s`. The first frame needed is
at `3s`, so prefetch seeks there before starting the decoder. Starting at `0s`
would prepare frames that may be discarded when the video becomes visible.

This initial seek applies when creating a player. A player whose resources
are already ready keeps its position. Loops that need the file duration to
calculate their seek target keep their initial position while the duration
is requested in the background.

Other resources, such as textures, are allocated during prefetch and freed
during release.


## Update

The 3rd pass is executing a cascading update with `ngli_node_update()`. Each
node decides how it updates its state and trigger the update of its children
for a given time.


## Draw

The 4th and final pass executed by `ngli_node_draw()` is similar to update in
the way it is cascading (explicit in every node). This pass does not require
time, its only purpose lies in drawing to the screen.
