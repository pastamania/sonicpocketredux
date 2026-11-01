# Reverse-engineering notes

Human-written observations about how the game works, gathered with runtime
traces and ValleyBell's disassembly. Addresses are cartridge (`0x2xxxxx`,
`0x3xxxxx`) or RAM (`0x4000`-`0x6FFF`) addresses.

| Note | Covers |
| --- | --- |
| [memory-map.md](memory-map.md) | Neo Geo Pocket Color address space |
| [entrypoint.md](entrypoint.md) | Cartridge entry point and startup |
| [vblank.md](vblank.md) | VBlank interrupt handler and its per-frame work |
| [input.md](input.md) | Controller polling and pause input |
| [task-scheduler.md](task-scheduler.md) | Cooperative task scheduler and task records |
| [player.md](player.md) | Player task fields, states and physics |
| [collision.md](collision.md) | Player collision pipeline and angle-aware sensors |
| [camera.md](camera.md) | Camera origin, stage bounds and follow logic |
| [sprites.md](sprites.md) | Sprite object list, how it becomes hardware sprites, and who owns each entry |
| [level-map.md](level-map.md) | Level header, layouts and block table, how the game fills the tile map, and the wide view |

These notes are the starting point for rewriting systems as structured C++ on
top of the recompiled game.
