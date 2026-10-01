# Jump straight to a galaxy

Start the game with `--stage` and, optionally, `--scenario` (the mission
number, default 1):

```sh
./Play\ Petari.command --stage good-egg --scenario 2
# or
build/macos-gx/native/app/Petari.app/Contents/MacOS/Petari --disc build/game-data/RMGE01 --stage good-egg --scenario 2
```

The game starts as usual. Pick your file at the file select; instead of the
Comet Observatory it takes you straight into that galaxy and mission. From then
on everything is the game's own: stars you collect are saved as usual, and
leaving the galaxy (or collecting its star) returns you to the observatory.

- **Your save is respected.** The jump changes no progress or unlock flags. It
  happens once per session, on the first file you load. A file that has not
  reached the Comet Observatory yet (still in the prologue or the Gateway
  tutorial) ignores the jump and starts normally.
- **Any galaxy, any mission**, including comet missions (C below) and hidden
  stars (H), whether or not the file has unlocked it. Collecting a star there
  saves that star as the game normally would. Bowser's Galaxy Reactor and the
  Grand Finale run the ending as usual.
- **Names.** Use an alias from the table, the internal stage name, or the
  galaxy's English name (`good-egg`, `EggStarGalaxy`, `"Good Egg Galaxy"`;
  case, spaces and punctuation are ignored). `--stage list` prints this table.
- A wrong name or mission number stops at launch with a message.

| Dome | Galaxy | `--stage` alias | Internal name | Missions |
| --- | --- | --- | --- | --- |
| Terrace | Bowser Jr.'s Robot Reactor | `bowser-jrs-robot-reactor` | TriLegLv1Galaxy | 1 |
| Terrace | Flipswitch Galaxy | `flipswitch` | FlipPanelExGalaxy | 1 |
| Terrace | Good Egg Galaxy | `good-egg` | EggStarGalaxy | 1 2 3 4C 5C 6H |
| Terrace | Honeyhive Galaxy | `honeyhive` | HoneyBeeKingdomGalaxy | 1 2 3 4C 5C 6H |
| Terrace | Loopdeeloop Galaxy | `loopdeeloop` | SurfingLv1Galaxy | 1 |
| Terrace | Sweet Sweet Galaxy | `sweet-sweet` | BeltConveyerExGalaxy | 1 |
| Fountain | Battlerock Galaxy | `battlerock` | BattleShipGalaxy | 1 2 3 4C 5C 6H 7H |
| Fountain | Bowser's Star Reactor | `bowsers-star-reactor` | KoopaBattleVs1Galaxy | 1 |
| Fountain | Hurry-Scurry Galaxy | `hurry-scurry` | BreakDownPlanetGalaxy | 1 |
| Fountain | Rolling Green Galaxy | `rolling-green` | TamakoroExLv1Galaxy | 1 |
| Fountain | Sling Pod Galaxy | `sling-pod` | CocoonExGalaxy | 1 |
| Fountain | Space Junk Galaxy | `space-junk` | StarDustGalaxy | 1 2 3 4C 5C 6H |
| Kitchen | Beach Bowl Galaxy | `beach-bowl` | HeavenlyBeachGalaxy | 1 2 3 4C 5C 6H |
| Kitchen | Bowser Jr.'s Airship Armada | `bowser-jrs-airship-armada` | KoopaJrShipLv1Galaxy | 1 |
| Kitchen | Bubble Breeze Galaxy | `bubble-breeze` | CubeBubbleExLv1Galaxy | 1 |
| Kitchen | Buoy Base Galaxy | `buoy-base` | OceanFloaterLandGalaxy | 1 2H |
| Kitchen | Drip Drop Galaxy | `drip-drop` | TearDropGalaxy | 1 |
| Kitchen | Ghostly Galaxy | `ghostly` | PhantomGalaxy | 1 2 3 4C 5C 6H |
| Bedroom | Bigmouth Galaxy | `bigmouth` | FishTunnelGalaxy | 1 |
| Bedroom | Bowser's Dark Matter Plant | `bowsers-dark-matter-plant` | KoopaBattleVs2Galaxy | 1 |
| Bedroom | Dusty Dune Galaxy | `dusty-dune` | SandClockGalaxy | 1 2 3 4C 5C 6H 7H |
| Bedroom | Freezeflame Galaxy | `freezeflame` | IceVolcanoGalaxy | 1 2 3 4C 5C 6H |
| Bedroom | Gusty Garden Galaxy | `gusty-garden` | CosmosGardenGalaxy | 1 2 3 4C 5C 6H |
| Bedroom | Honeyclimb Galaxy | `honeyclimb` | HoneyBeeExGalaxy | 1 |
| Engine Room | Bonefin Galaxy | `bonefin` | SkullSharkGalaxy | 1 |
| Engine Room | Bowser Jr.'s Lava Reactor | `bowser-jrs-lava-reactor` | FloaterOtaKingGalaxy | 1 |
| Engine Room | Gold Leaf Galaxy | `gold-leaf` | ReverseKingdomGalaxy | 1 2 3 4C 5C 6H |
| Engine Room | Sand Spiral Galaxy | `sand-spiral` | TransformationExGalaxy | 1 |
| Engine Room | Sea Slide Galaxy | `sea-slide` | OceanRingGalaxy | 1 2 3 4C 5C 6H |
| Engine Room | Toy Time Galaxy | `toy-time` | FactoryGalaxy | 1 2 3 4C 5C 6H |
| Garden | Boo's Boneyard Galaxy | `boos-boneyard` | TeresaMario2DGalaxy | 1 |
| Garden | Deep Dark Galaxy | `deep-dark` | OceanPhantomCaveGalaxy | 1 2 3 4C 5C 6H |
| Garden | Dreadnought Galaxy | `dreadnought` | CannonFleetGalaxy | 1 2 3 4C 5C 6H |
| Garden | Matter Splatter Galaxy | `matter-splatter` | DarkRoomGalaxy | 1 |
| Garden | Melty Molten Galaxy | `melty-molten` | HellProminenceGalaxy | 1 2 3 4C 5C 6H |
| Garden | Snow Cap Galaxy | `snow-cap` | SnowCapsuleGalaxy | 1 |
| Elsewhere | Bowser's Galaxy Reactor | `bowsers-galaxy-reactor` | KoopaBattleVs3Galaxy | 1 |
| Elsewhere | Bubble Blast Galaxy | `bubble-blast` | CubeBubbleExLv2Galaxy | 1 |
| Elsewhere | Gateway Galaxy | `gateway` | HeavensDoorGalaxy | 1 2 |
| Elsewhere | Grand Finale Galaxy | `grand-finale` | PeachCastleFinalGalaxy | 1 |
| Elsewhere | Loopdeeswoop Galaxy | `loopdeeswoop` | SurfingLv2Galaxy | 1 |
| Elsewhere | Rolling Gizmo Galaxy | `rolling-gizmo` | TamakoroExLv2Galaxy | 1 |

Comet (C) and hidden-star (H) missions are numbered after the regular ones, in
the order the game's mission list uses (for example Good Egg: 4 is Dino Piranha
Speed Run, 5 Purple Coin Omelet, 6 Luigi on the Roof).
