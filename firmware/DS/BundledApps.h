#pragma once

// Generated from apps/adventure.dapp for firmware-side LittleFS seeding.
static const char BUNDLED_APP_ADVENTURE[] = R"DOLLAPP(# @dapp-format 1
# @id adventure
# @name The Cursed Grotto
# @version 1.0.0
# @boards fnk0104
# @runtime >=1.0.0 <2.0.0
# @summary A branching fantasy text adventure
# /sd/apps/adventure.dapp
# A Mini DOLL-OS Adventure Game - "The Cursed Grotto"
# Run with: apps run adventure

COLOR cyan
PRINT "=================================="
PRINT "   THE CURSED GROTTO"
PRINT "   A Mini DOLL-OS Adventure"
PRINT "=================================="
WAIT 500
CLEAR

COLOR white
PRINT "You stand before a moss-covered"
PRINT "stone archway leading deep into"
PRINT "an ancient cave system..."
WAIT 600
PRINT ""
PRINT "Rumor says a treasure lies deep"
PRINT "within, but danger lurks at every"
PRINT "turn. Will you enter?"
WAIT 500

:menu
PRINT ""
COLOR yellow
PRINT "Choose your path:"
COLOR white
PRINT "  1. Enter the Grotto"
PRINT "  2. Turn back"
INPUT choice "> "
IFEQ $choice 2 GOTO coward
IFEQ $choice 1 GOTO enter
COLOR magenta
PRINT "Hmm, try 1 or 2!"
GOTO menu

:coward
COLOR red
PRINT "You wisely turn back."
PRINT "Some say wisdom is just"
PRINT "another word for fear..."
WAIT 500
COLOR pink
PRINT "Better luck next time!"
PRINT "Score: 0"
EXIT

:enter
CLEAR
COLOR green
PRINT "You push through the archway"
PRINT "and descend into darkness..."
WAIT 700
CLEAR
COLOR white
PRINT "Inside the Cursed Grotto"
PRINT "========================="
PRINT ""
PRINT "The cave opens into a large"
PRINT "chamber with a faint blue"
PRINT "glow on the walls. Three"
PRINT "passages are visible:"
WAIT 600

SET gold 0
SET health 100
SET potions 1

:chamber1
PRINT ""
COLOR yellow
PRINT "1. Left - A narrow tunnel"
PRINT "2. Center - A wide corridor"
PRINT "3. Right - A crack in the wall"
INPUT choice "> "
IFEQ $choice 1 GOTO narrow
IFEQ $choice 2 GOTO wide
IFEQ $choice 3 GOTO crack
COLOR magenta
PRINT "Choose 1, 2, or 3!"
GOTO chamber1

:narrow
CLEAR
COLOR white
PRINT "The Narrow Tunnel"
PRINT "=================="
PRINT ""
PRINT "You squeeze through the tight"
PRINT "passage. Rocks scrape your"
PRINT "arms and something scratches!"
ADD health -10
COLOR red
PRINT "* Health: $health *"
WAIT 400
PRINT ""
COLOR cyan
PRINT "But at the end, you find a"
PRINT "shimmering crystal! It pulses"
PRINT "with magical energy."
SETSTR item "Crystal of Light"
COLOR green
PRINT "* Found: $item *"
ADD gold 50
COLOR yellow
PRINT "* Gold: $gold *"
PRINT ""
COLOR white
PRINT "The tunnel leads to a fork:"
COLOR yellow
PRINT "1. Take the high path"
PRINT "2. Take the low path"
INPUT choice "> "
IFEQ $choice 1 GOTO highpath
IFEQ $choice 2 GOTO lowpath
COLOR magenta
PRINT "Choose 1 or 2!"
GOTO narrow

:highpath
CLEAR
COLOR white
PRINT "The High Path"
PRINT "=============="
PRINT ""
PRINT "You climb over rocky ledges"
PRINT "and find yourself on a narrow"
PRINT "bridge over a chasm!"
WAIT 600
PRINT ""
COLOR cyan
PRINT "On the bridge, a hooded figure"
PRINT "stands before you."
WAIT 400
PRINT ""
COLOR magenta
PRINT "Trader: 'Greetings, adventurer!"
PRINT "I have something you might need.'"
PRINT "A healing potion for 30 gold?'"
WAIT 600
PRINT ""
COLOR yellow
PRINT "1. Buy potion (30g)"
PRINT "2. Decline"
INPUT choice "> "
IFEQ $choice 1 GOTO buy
IFEQ $choice 2 GOTO decline
COLOR magenta
PRINT "Choose 1 or 2!"
GOTO highpath

:decline
COLOR white
PRINT "You keep your gold and move on."
WAIT 400
GOTO continuebridge

:buy
IF $gold <= 30 GOTO toopoor
ADD gold -30
ADD potions 1
COLOR green
PRINT "* Bought a potion! *"
COLOR yellow
PRINT "* Gold: $gold | Potions: $potions *"
GOTO continuebridge

:toopoor
COLOR red
PRINT "Trader: 'Ah, not enough gold!"
PRINT "Come back richer, friend!'"
WAIT 400
PRINT ""

:continuebridge
PRINT "The trader nods and vanishes."
COLOR white
PRINT "You cross the bridge safely"
PRINT "and find a small chest!"
WAIT 500
PRINT ""
COLOR yellow
PRINT "1. Open the chest"
PRINT "2. Leave it (trap?)"
INPUT choice "> "
IFEQ $choice 1 GOTO openchest
IFEQ $choice 2 GOTO skipchest
COLOR magenta
PRINT "Choose 1 or 2!"
GOTO continuebridge

:openchest
RAND trap 1 6
IF $trap >= 4 GOTO safechest
GOTO trapped

:trapped
COLOR red
PRINT "* A trap! The chest sprays"
PRINT "poisonous spores! *"
ADD health -25
COLOR red
PRINT "* Health: $health *"
WAIT 600
IF $health <= 0 GOTO gameover
PRINT ""
COLOR white
PRINT "You stumble onward..."
GOTO afterchest

:safechest
COLOR green
PRINT "* The chest is safe! *"
ADD gold 75
COLOR yellow
PRINT "* Found 75 gold! Total: $gold *"
WAIT 500
GOTO afterchest

:skipchest
COLOR white
PRINT "You leave the chest behind"
PRINT "and move onward wisely."
WAIT 400

:afterchest
CLEAR
COLOR white
PRINT "The bridge leads to an ancient"
PRINT "altar with a carved stone:"
PRINT ""
COLOR cyan
PRINT "'The one who holds light"
PRINT "shall pass, but beware"
PRINT "the shadows below.'"
WAIT 700
PRINT ""
COLOR yellow
PRINT "1. Continue to the depths"
PRINT "2. Return to the chamber"
INPUT choice "> "
IFEQ $choice 1 GOTO depths
IFEQ $choice 2 GOTO chamber1
COLOR magenta
PRINT "Choose 1 or 2!"
GOTO afterchest

:wide
CLEAR
COLOR white
PRINT "The Wide Corridor"
PRINT "==================="
PRINT ""
PRINT "This spacious passage is lined"
PRINT "with strange murals depicting"
PRINT "an ancient civilization."
WAIT 600
PRINT ""
COLOR cyan
PRINT "One mural shows a figure holding"
PRINT "a glowing orb toward a sealed"
PRINT "door marked with three symbols."
WAIT 500
PRINT ""
COLOR yellow
PRINT "1. Study the murals"
PRINT "2. Continue forward"
INPUT choice "> "
IFEQ $choice 1 GOTO study
IFEQ $choice 2 GOTO forward
COLOR magenta
PRINT "Choose 1 or 2!"
GOTO wide

:study
CLEAR
COLOR white
PRINT "You examine the murals closely."
WAIT 400
PRINT ""
COLOR cyan
PRINT "They tell the story of a guardian"
PRINT "beast and how only those with"
PRINT "a 'light source' can pass safely."
WAIT 600
PRINT ""
COLOR green
PRINT "* You learn about the guardian! *"
SETSTR knowledge "Guardian weak to light"
WAIT 500
PRINT ""
COLOR yellow
PRINT "1. Continue forward"
PRINT "2. Return to the chamber"
INPUT choice "> "
IFEQ $choice 1 GOTO forward
IFEQ $choice 2 GOTO chamber1
COLOR magenta
PRINT "Choose 1 or 2!"
GOTO study

:forward
CLEAR
COLOR white
PRINT "You press onward through the"
PRINT "corridor. A low growl echoes"
PRINT "from ahead..."
WAIT 700
PRINT ""
COLOR red
PRINT "* A massive cave bat swoops"
PRINT "down at you! *"
WAIT 500
PRINT ""
COLOR yellow
PRINT "1. Fight it!"
PRINT "2. Use a potion"
INPUT choice "> "
IFEQ $choice 1 GOTO fightbat
IFEQ $choice 2 GOTO usepotionbat
COLOR magenta
PRINT "Choose 1 or 2!"
GOTO forward

:fightbat
CLEAR
RAND damage 15 40
COLOR white
PRINT "You swing wildly at the bat!"
PRINT "Its wings block some attacks."
WAIT 500
PRINT ""
COLOR red
PRINT "* The bat strikes back! *"
ADD health -$damage
COLOR red
PRINT "* Health: $health *"
WAIT 400
IF $health <= 0 GOTO gameover
PRINT ""
COLOR green
PRINT "But you drive the bat away!"
PRINT "It screeches and flees."
WAIT 500
GOTO guardian

:usepotionbat
ADD potions -1
IF $potions < 0 GOTO nopotion
COLOR white
PRINT "You toss a potion to create a"
PRINT "smokescreen and escape!"
ADD health 15
COLOR green
PRINT "* Health restored to: $health *"
COLOR yellow
PRINT "* Potions: $potions *"
WAIT 500
GOTO guardian

:nopotion
COLOR red
PRINT "No potions left!"
ADD health -30
COLOR red
PRINT "* Health: $health *"
IF $health <= 0 GOTO gameover
COLOR white
PRINT "You barely escape!"
WAIT 400
GOTO guardian

:guardian
CLEAR
COLOR white
PRINT "You arrive before a massive"
PRINT "sealed door. The guardian"
PRINT "legend speaks of appears!"
WAIT 700
PRINT ""
COLOR red
PRINT "* A shadow serpent coils"
PRINT "before the door! *"
WAIT 600
PRINT ""
COLOR yellow
PRINT "1. Attack head-on!"
PRINT "2. Use your crystal"
INPUT choice "> "
IFEQ $choice 1 GOTO attacksnake
IFEQ $choice 2 GOTO usecrystal
COLOR magenta
PRINT "Choose 1 or 2!"
GOTO guardian

:attacksnake
COLOR red
PRINT "You charge the serpent!"
ADD health -40
COLOR red
PRINT "* Health: $health *"
WAIT 400
IF $health <= 0 GOTO gameover
COLOR white
PRINT "You wound it, but it strikes"
PRINT "back viciously!"
WAIT 500
PRINT ""
COLOR yellow
PRINT "1. Press the attack"
PRINT "2. Retreat to chamber"
INPUT choice "> "
IFEQ $choice 1 GOTO pressattack
IFEQ $choice 2 GOTO chamber1
COLOR magenta
PRINT "Choose 1 or 2!"
GOTO attacksnake

:pressattack
COLOR white
PRINT "You strike with all your might!"
COLOR green
PRINT "The serpent recoils and"
PRINT "the door unlocks behind it!"
ADD gold 100
COLOR yellow
PRINT "* Gold: $gold *"
WAIT 500
GOTO treasure_room

:usecrystal
COLOR cyan
PRINT "You hold up the Crystal of Light!"
COLOR white
PRINT "The serpent hisses and writhes"
PRINT "as the light weakens it!"
WAIT 700
COLOR green
PRINT "* The serpent retreats into"
PRINT "the shadows, defeated! *"
WAIT 500
COLOR white
PRINT "The door slides open before you."
GOTO treasure_room

:lowpath
CLEAR
COLOR white
PRINT "The Low Path"
PRINT "============="
PRINT ""
PRINT "You descend into a flooded"
PRINT "passage. Cold water laps at"
PRINT "your ankles."
WAIT 600
PRINT ""
COLOR cyan
PRINT "The water glows faintly. Fish"
PRINT "with luminous scales dart"
PRINT "past you."
WAIT 500
PRINT ""
COLOR yellow
PRINT "1. Search the underwater crevices"
PRINT "2. Keep moving forward"
INPUT choice "> "
IFEQ $choice 1 GOTO searchwater
IFEQ $choice 2 GOTO keepmoving
COLOR magenta
PRINT "Choose 1 or 2!"
GOTO lowpath

:searchwater
COLOR white
PRINT "You dive into the crevices..."
WAIT 500
RAND find 1 5
IF $find >= 3 GOTO foundgold
COLOR cyan
PRINT "You find some glowing pearls!"
ADD gold 40
COLOR yellow
PRINT "* Found 40 gold! Total: $gold *"
WAIT 500
GOTO afterwater

:foundgold
COLOR yellow
PRINT "You uncover an old stash of coins!"
ADD gold 60
PRINT "* Found 60 gold! Total: $gold *"
WAIT 500
GOTO afterwater

:keepmoving
COLOR white
PRINT "You wade through the passage"
PRINT "carefully, avoiding the deeper"
PRINT "sections."
WAIT 500
PRINT ""
COLOR white
PRINT "The passage ends at a small"
PRINT "underground pool."
WAIT 400

:afterwater
CLEAR
COLOR white
PRINT "You emerge from the water at a"
PRINT "stone staircase leading upward."
WAIT 500
PRINT ""
COLOR yellow
PRINT "1. Climb the stairs"
PRINT "2. Return to the chamber"
INPUT choice "> "
IFEQ $choice 1 GOTO climbstairs
IFEQ $choice 2 GOTO chamber1
COLOR magenta
PRINT "Choose 1 or 2!"
GOTO afterwater

:climbstairs
CLEAR
COLOR white
PRINT "You climb the ancient stairs."
PRINT "At the top, a massive door"
PRINT "blocks your path!"
WAIT 600
PRINT ""
COLOR yellow
PRINT "1. Push the door"
PRINT "2. Look for another way"
INPUT choice "> "
IFEQ $choice 1 GOTO pushdoor
IFEQ $choice 2 GOTO findlever
COLOR magenta
PRINT "Choose 1 or 2!"
GOTO climbstairs

:pushdoor
COLOR white
PRINT "You strain against the door..."
ADD health -15
COLOR red
PRINT "* Health: $health *"
WAIT 400
IF $health <= 0 GOTO gameover
COLOR white
PRINT "It cracks open just enough!"
WAIT 500
GOTO treasure_room

:findlever
COLOR white
PRINT "You search the walls and find"
PRINT "a hidden lever! It clicks"
PRINT "smoothly and the door swings open."
WAIT 700
GOTO treasure_room

:crack
CLEAR
COLOR white
PRINT "The Crack in the Wall"
PRINT "====================="
PRINT ""
PRINT "You squeeze through a narrow"
PRINT "crack. It's tight but you make"
PRINT "it through!"
ADD health -5
COLOR red
PRINT "* Health: $health *"
WAIT 400
IF $health <= 0 GOTO gameover
PRINT ""
COLOR white
PRINT "You find yourself in a hidden"
PRINT "nook with a small altar."
WAIT 600
PRINT ""
COLOR cyan
PRINT "On the altar sits a potion!"
COLOR yellow
PRINT "1. Take the potion"
INPUT choice "> "
ADD potions 1
COLOR green
PRINT "* Potion acquired! *"
COLOR yellow
PRINT "* Potions: $potions *"
WAIT 500
PRINT ""
COLOR white
PRINT "The nook has one exit leading"
PRINT "down into darkness."
COLOR yellow
PRINT "1. Descend"
INPUT choice "> "
GOTO depths

:depths
CLEAR
COLOR white
PRINT "The Depths"
PRINT "==========="
PRINT ""
COLOR red
PRINT "The air grows cold. Strange"
PRINT "whispers echo from the walls."
WAIT 700
PRINT ""
COLOR cyan
PRINT "You see a faint glow ahead."
PRINT "As you approach, you discover"
PRINT "the heart of the grotto!"
WAIT 600
PRINT ""
COLOR yellow
PRINT "1. Enter the glowing chamber"
PRINT "2. Turn back to safety"
INPUT choice "> "
IFEQ $choice 1 GOTO treasure_room
IFEQ $choice 2 GOTO chamber1
COLOR magenta
PRINT "Choose 1 or 2!"
GOTO depths

:treasure_room
CLEAR
COLOR pink
PRINT "=================================="
PRINT "    THE TREASURE CHAMBER"
PRINT "=================================="
WAIT 700
CLEAR
COLOR white
PRINT "Golden light fills the chamber."
PRINT "Shelves of gold and gemstones"
PRINT "line the walls!"
WAIT 700
PRINT ""
COLOR yellow
PRINT "But as you reach for the gold,"
PRINT "the floor begins to rumble..."
WAIT 600
PRINT ""
COLOR red
PRINT "THE GROTTO IS COLLAPSING!"
WAIT 500
PRINT ""
COLOR yellow
PRINT "1. Grab gold and run!"
PRINT "2. Search for the legendary orb"
INPUT choice "> "
IFEQ $choice 1 GOTO grabgold
IFEQ $choice 2 GOTO findorb
COLOR magenta
PRINT "Choose 1 or 2!"
GOTO treasure_room

:grabgold
COLOR white
PRINT "You stuff your pockets with gold!"
WAIT 400
ADD gold 200
COLOR yellow
PRINT "* +200 Gold! Total: $gold *"
WAIT 500
GOTO escape

:findorb
COLOR cyan
PRINT "You spot a floating orb of"
PRINT "pure light! The source of the"
PRINT "grotto's power!"
WAIT 700
COLOR green
PRINT "* The Orb of the Grotto! *"
PRINT "It burns in your hands with"
PRINT "immense power!"
ADD gold 300
COLOR yellow
PRINT "* +300 Gold! Total: $gold *"
WAIT 500
GOTO escape

:escape
CLEAR
COLOR white
PRINT "You sprint through crumbling"
PRINT "passages, dodging falling"
PRINT "rocks!"
WAIT 700
COLOR red
PRINT "A massive boulder blocks"
PRINT "the main exit!"
WAIT 500
PRINT ""
COLOR yellow
PRINT "1. Squeeze through a side passage"
PRINT "2. Try to move the boulder"
INPUT choice "> "
IFEQ $choice 1 GOTO squeeze
IFEQ $choice 2 GOTO moveboulder
COLOR magenta
PRINT "Choose 1 or 2!"
GOTO escape

:squeeze
COLOR white
PRINT "You squeeze through a narrow"
PRINT "gap beside the boulder!"
ADD health -20
COLOR red
PRINT "* Health: $health *"
WAIT 400
IF $health <= 0 GOTO gameover
COLOR green
PRINT "You burst out into fresh air!"
WAIT 500
GOTO victory

:moveboulder
COLOR white
PRINT "You heave at the boulder with"
PRINT "all your might!"
ADD health -30
COLOR red
PRINT "* Health: $health *"
WAIT 400
IF $health <= 0 GOTO gameover
COLOR white
PRINT "It shifts! You scramble through"
PRINT "the opening as the cave"
PRINT "collapses behind you!"
WAIT 700
GOTO victory

:victory
CLEAR
COLOR green
PRINT "=================================="
PRINT "        VICTORY!"
PRINT "=================================="
WAIT 500
PRINT ""
COLOR white
PRINT "You stand outside the grotto,"
PRINT "covered in dust but alive!"
PRINT "The entrance crumbles and seals"
PRINT "behind you."
WAIT 700
PRINT ""
COLOR cyan
PRINT "Your adventure in the Cursed"
PRINT "Grotto is complete!"
WAIT 500
PRINT ""
COLOR yellow
PRINT "=============================="
PRINT "  Final Score:"
PRINT "  Gold: $gold"
PRINT "  Health: $health"
PRINT "  Potions left: $potions"
PRINT "=============================="
WAIT 700

IF $gold >= 400 GOTO perfect
IF $gold >= 200 GOTO great
IF $gold >= 100 GOTO good
GOTO okay

:perfect
COLOR green
PRINT ""
PRINT "*** LEGENDARY ADVENTURER! ***"
PRINT "You explored every corner and"
PRINT "earned a fortune!"
WAIT 600
GOTO final

:great
COLOR cyan
PRINT ""
PRINT "*** EXCELLENT ADVENTURER! ***"
PRINT "You navigated the grotto with"
PRINT "skill and wit!"
WAIT 600
GOTO final

:good
COLOR yellow
PRINT ""
PRINT "*** BRAVE ADVENTURER! ***"
PRINT "You survived and found treasure!"
WAIT 600
GOTO final

:okay
COLOR white
PRINT ""
PRINT "*** CAUTIOUS ADVENTURER ***"
PRINT "You survived - some would call"
PRINT "that victory enough!"
WAIT 600

:final
PRINT ""
COLOR pink
PRINT "Thanks for playing!"
PRINT "Run 'apps run adventure'"
PRINT "to play again!"
WAIT 750
EXIT

:gameover
CLEAR
COLOR red
PRINT "=================================="
PRINT "      GAME OVER"
PRINT "=================================="
WAIT 500
PRINT ""
COLOR white
PRINT "Your health has fallen to zero."
PRINT "The Cursed Grotto claims another"
PRINT "soul..."
WAIT 700
PRINT ""
COLOR yellow
PRINT "Gold collected: $gold"
PRINT "Potions remaining: $potions"
WAIT 700
PRINT ""
COLOR pink
PRINT "Thanks for playing!"
PRINT "Run 'apps run adventure'"
PRINT "to try again!"
WAIT 750
EXIT
)DOLLAPP";

// Generated from apps/tetris.dapp for firmware-side LittleFS seeding.
static const char BUNDLED_APP_TETRIS[] = R"DOLLAPP(# @dapp-format 1
# @id tetris
# @name Tetris
# @version 1.0.0
# @boards fnk0104
# @runtime >=1.1.0 <2.0.0
# @summary A falling-block puzzle game for the DOLL-OS canvas
# /apps/tetris.dapp
# Tetris for the DOLL-OS shell. Arrows or WASD to move, up/W to rotate,
# space to hard drop, escape to quit.
#
# This is the app the .dapp language grew CANVAS/KEY/DIM/EXPR/GOSUB for --
# every one of those is load-bearing here. The well is a 200-cell array,
# the piece tables are three more, gravity runs off $millis rather than
# blocking on INPUT, and the whole frame is drawn by cell instead of
# printed as lines.

CANVAS 32 22
COLOR white
PUT 8 10 "TETRIS"
PUT 4 12 "arrows move - up rotates"
PUT 5 13 "space drops - esc quits"
FLIP
WAIT 1200

# Piece geometry. Pieces are 0..6 = I O T S Z J L, each stored as the four
# cells of its spawn rotation inside an NxN box; every other rotation is
# computed from those (see cellat), so there is no 7x4 shape table to keep.
DIM pbox 7
SET pbox[0] 4
SET pbox[1] 2
SET pbox[2] 3
SET pbox[3] 3
SET pbox[4] 3
SET pbox[5] 3
SET pbox[6] 3

# row of each cell, indexed piece*4 + cell
DIM pr 28
SET pr[0] 1
SET pr[1] 1
SET pr[2] 1
SET pr[3] 1
SET pr[4] 0
SET pr[5] 0
SET pr[6] 1
SET pr[7] 1
SET pr[8] 0
SET pr[9] 1
SET pr[10] 1
SET pr[11] 1
SET pr[12] 0
SET pr[13] 0
SET pr[14] 1
SET pr[15] 1
SET pr[16] 0
SET pr[17] 0
SET pr[18] 1
SET pr[19] 1
SET pr[20] 0
SET pr[21] 1
SET pr[22] 1
SET pr[23] 1
SET pr[24] 0
SET pr[25] 1
SET pr[26] 1
SET pr[27] 1

# column of each cell, same indexing
DIM pc 28
SET pc[0] 0
SET pc[1] 1
SET pc[2] 2
SET pc[3] 3
SET pc[4] 0
SET pc[5] 1
SET pc[6] 0
SET pc[7] 1
SET pc[8] 1
SET pc[9] 0
SET pc[10] 1
SET pc[11] 2
SET pc[12] 1
SET pc[13] 2
SET pc[14] 0
SET pc[15] 1
SET pc[16] 0
SET pc[17] 1
SET pc[18] 1
SET pc[19] 2
SET pc[20] 0
SET pc[21] 0
SET pc[22] 1
SET pc[23] 2
SET pc[24] 2
SET pc[25] 0
SET pc[26] 1
SET pc[27] 2

# the well: 10 wide, 20 tall, 0 = empty, else the piece's color index
DIM well 200

SET score 0
SET lines 0
SET level 1
SET dropms 650
SET quit 0
SET dirty 1

# best score so far, persisted in /apps/tetris.hs across runs
GOSUB loadhs

GOTO spawn

# ---------------------------------------------------------------- main loop

:loop
IF $dirty = 1 GOSUB draw
SET dirty 0
KEY k
IF $k <> 0 GOSUB handlekey
IF $quit = 1 GOTO leave
EXPR remain $dropat - $millis
IF $remain > 0 GOTO loop_wait
GOSUB gravity
IF $locked = 1 GOTO spawn
:loop_wait
WAIT 12
GOTO loop

:spawn
RAND piece 7
SET rot 0
SET px 3
SET py 0
GOSUB collide
IF $hit = 1 GOTO gameover
EXPR dropat $millis + $dropms
SET dirty 1
GOTO loop

# ---------------------------------------------------------------- input

:handlekey
IF $k = $kesc GOTO hk_quit
IF $k = $kleft GOTO hk_left
IF $k = 97 GOTO hk_left
IF $k = $kright GOTO hk_right
IF $k = 100 GOTO hk_right
IF $k = $kup GOTO hk_rot
IF $k = 119 GOTO hk_rot
IF $k = $kdown GOTO hk_down
IF $k = 115 GOTO hk_down
IF $k = $kspace GOTO hk_drop
RETURN

:hk_quit
SET quit 1
RETURN

:hk_left
SUB px 1
GOSUB collide
IF $hit = 0 GOTO hk_left_ok
ADD px 1
:hk_left_ok
SET dirty 1
RETURN

:hk_right
ADD px 1
GOSUB collide
IF $hit = 0 GOTO hk_right_ok
SUB px 1
:hk_right_ok
SET dirty 1
RETURN

# rotate, then try the two one-cell wall kicks before giving up
:hk_rot
SET oldrot $rot
SET oldpx $px
ADD rot 1
IF $rot < 4 GOTO hk_rot_test
SET rot 0
:hk_rot_test
GOSUB collide
IF $hit = 0 GOTO hk_rot_ok
ADD px 1
GOSUB collide
IF $hit = 0 GOTO hk_rot_ok
SUB px 2
GOSUB collide
IF $hit = 0 GOTO hk_rot_ok
SET px $oldpx
SET rot $oldrot
:hk_rot_ok
SET dirty 1
RETURN

:hk_down
ADD py 1
GOSUB collide
IF $hit = 0 GOTO hk_down_ok
SUB py 1
SET dropat 0
RETURN
:hk_down_ok
EXPR dropat $millis + $dropms
ADD score 1
SET dirty 1
RETURN

# hard drop parks the piece and zeroes the gravity timer, so the locking
# itself happens in the one place that does it -- the main loop's gravity
:hk_drop
ADD py 1
GOSUB collide
IF $hit = 0 GOTO hk_drop
SUB py 1
SET dropat 0
SET dirty 1
RETURN

# ---------------------------------------------------------------- rules

# one gravity step. Sets locked=1 when the piece came to rest, and never
# jumps out of itself -- a GOTO out of a GOSUB would strand its return address.
:gravity
SET locked 0
ADD py 1
GOSUB collide
IF $hit = 0 GOTO gr_done
SUB py 1
GOSUB lock
GOSUB clearlines
GOSUB addscore
SET locked 1
:gr_done
EXPR dropat $millis + $dropms
SET dirty 1
RETURN

# cell $i of the current piece at the current rotation, as cr/cc within its box.
# Rotating clockwise in an NxN box is (r,c) -> (c, N-1-r), applied $rot times.
:cellat
EXPR pi $piece * 4 + $i
SET cr $pr[$pi]
SET cc $pc[$pi]
SET rleft $rot
:cellat_spin
IF $rleft <= 0 GOTO cellat_done
SET t $cr
SET cr $cc
EXPR cc $pbox[$piece] - 1 - $t
SUB rleft 1
GOTO cellat_spin
:cellat_done
RETURN

# hit=1 if the piece at px/py/rot leaves the well or overlaps a settled cell.
# Rows above the top are legal, which is what lets a piece spawn partly offscreen.
:collide
SET hit 0
SET i 0
:collide_cell
GOSUB cellat
EXPR bx $px + $cc
EXPR by $py + $cr
IF $bx < 0 GOTO collide_hit
IF $bx > 9 GOTO collide_hit
IF $by > 19 GOTO collide_hit
IF $by < 0 GOTO collide_next
EXPR wi $by * 10 + $bx
IF $well[$wi] <> 0 GOTO collide_hit
:collide_next
ADD i 1
IF $i < 4 GOTO collide_cell
RETURN
:collide_hit
SET hit 1
RETURN

:lock
SET i 0
EXPR lc $piece + 1
:lock_cell
GOSUB cellat
EXPR bx $px + $cc
EXPR by $py + $cr
IF $by < 0 GOTO lock_next
EXPR wi $by * 10 + $bx
SET well[$wi] $lc
:lock_next
ADD i 1
IF $i < 4 GOTO lock_cell
RETURN

# collapses every full row, bottom up, leaving the count in `cleared`
:clearlines
SET cleared 0
SET ry 19
:cl_row
IF $ry < 0 GOTO cl_done
SET cx 0
SET full 1
:cl_scan
EXPR wi $ry * 10 + $cx
IF $well[$wi] <> 0 GOTO cl_scan_next
SET full 0
:cl_scan_next
ADD cx 1
IF $cx < 10 GOTO cl_scan
IF $full = 0 GOTO cl_next_row
SET sy $ry
:cl_shift
IF $sy <= 0 GOTO cl_top
SET cx 0
:cl_shift_cell
EXPR di $sy * 10 + $cx
EXPR si $di - 10
SET well[$di] $well[$si]
ADD cx 1
IF $cx < 10 GOTO cl_shift_cell
SUB sy 1
GOTO cl_shift
:cl_top
SET cx 0
:cl_top_cell
SET well[$cx] 0
ADD cx 1
IF $cx < 10 GOTO cl_top_cell
ADD cleared 1
GOTO cl_row
:cl_next_row
SUB ry 1
GOTO cl_row
:cl_done
RETURN

:addscore
IF $cleared = 0 GOTO as_done
ADD lines $cleared
IF $cleared = 1 GOTO as_one
IF $cleared = 2 GOTO as_two
IF $cleared = 3 GOTO as_three
EXPR score $score + 800 * $level
GOTO as_level
:as_one
EXPR score $score + 100 * $level
GOTO as_level
:as_two
EXPR score $score + 300 * $level
GOTO as_level
:as_three
EXPR score $score + 500 * $level
:as_level
EXPR level floor($lines / 10) + 1
EXPR dropms 650 - ($level - 1) * 55
IF $dropms > 80 GOTO as_done
SET dropms 80
:as_done
RETURN

# ---------------------------------------------------------------- drawing

:draw
CLS
COLOR blue
PUT 0 0 "+--------------------+"
PUT 0 21 "+--------------------+"
SET dy 1
:draw_side
PUT 0 $dy "|"
PUT 21 $dy "|"
ADD dy 1
IF $dy < 21 GOTO draw_side

SET dy 0
:draw_row
SET dx 0
:draw_col
EXPR wi $dy * 10 + $dx
SET ck $well[$wi]
IF $ck = 0 GOTO draw_col_next
GOSUB setcolor
EXPR sx $dx * 2 + 1
EXPR sy $dy + 1
PUT $sx $sy "[]"
:draw_col_next
ADD dx 1
IF $dx < 10 GOTO draw_col
ADD dy 1
IF $dy < 20 GOTO draw_row

EXPR ck $piece + 1
GOSUB setcolor
SET i 0
:draw_piece
GOSUB cellat
EXPR bx $px + $cc
EXPR by $py + $cr
IF $by < 0 GOTO draw_piece_next
EXPR sx $bx * 2 + 1
EXPR sy $by + 1
PUT $sx $sy "[]"
:draw_piece_next
ADD i 1
IF $i < 4 GOTO draw_piece

COLOR pink
PUT 23 1 "TETRIS"
COLOR cyan
PUT 23 3 "SCORE"
COLOR white
PUT 23 4 $score
COLOR cyan
PUT 23 6 "LINES"
COLOR white
PUT 23 7 $lines
COLOR cyan
PUT 23 9 "LEVEL"
COLOR white
PUT 23 10 $level
COLOR cyan
PUT 23 12 "HI"
COLOR white
PUT 23 13 $hiscore
COLOR yellow
PUT 23 14 "<- ->"
PUT 23 15 "move"
PUT 23 16 "up rot"
PUT 23 17 "dn soft"
PUT 23 18 "spc drop"
PUT 23 20 "esc quit"
FLIP
RETURN

# COLOR takes a name, so the color index in a well cell has to be dispatched
:setcolor
IF $ck = 1 GOTO scol_i
IF $ck = 2 GOTO scol_o
IF $ck = 3 GOTO scol_t
IF $ck = 4 GOTO scol_s
IF $ck = 5 GOTO scol_z
IF $ck = 6 GOTO scol_j
COLOR pink
RETURN
:scol_i
COLOR cyan
RETURN
:scol_o
COLOR yellow
RETURN
:scol_t
COLOR magenta
RETURN
:scol_s
COLOR green
RETURN
:scol_z
COLOR red
RETURN
:scol_j
COLOR blue
RETURN

# ---------------------------------------------------------------- high score

# reads /apps/tetris.hs into hiscore, tolerating a missing or garbled file --
# a fresh install simply starts the ladder at 0
:loadhs
SET hiscore 0
FEXISTS hs "/apps/tetris.hs"
IF $hs = 0 GOTO lh_done
FOPEN "/apps/tetris.hs" read
IF $fok = 0 GOTO lh_done
FREAD text
FCLOSE
GOSUB str2num
SET hiscore $num
:lh_done
RETURN

# in: text (string)   out: num -- digits off the front of a string, since a
# number read back from a file is text again (the Chapter 5 routine)
:str2num
SET num 0
SET s2i 0
LEN s2len $text
:s2n_loop
IF $s2i >= $s2len GOTO s2n_done
CHARAT s2d $text $s2i
IF $s2d < 48 GOTO s2n_done
IF $s2d > 57 GOTO s2n_done
SUB s2d 48
MUL num 10
ADD num $s2d
ADD s2i 1
GOTO s2n_loop
:s2n_done
RETURN

# ---------------------------------------------------------------- exits

:gameover
IF $score <= $hiscore GOTO go_banner
SET hiscore $score
FOPEN "/apps/tetris.hs" write
IF $fok = 0 GOTO go_nosave
FWRITE $score
FCLOSE
:go_nosave
COLOR yellow
PUT 4 8 "NEW HIGH SCORE"
:go_banner
COLOR red
PUT 6 10 "GAME OVER"
COLOR white
PUT 5 12 "score $score"
FLIP
WAIT 2500

:leave
ENDCANVAS
COLOR pink
PRINT "tetris: $lines lines, level $level, score $score (best $hiscore)"
EXIT
)DOLLAPP";

// Generated from apps/snake.dapp for firmware-side LittleFS seeding.
static const char BUNDLED_APP_SNAKE[] = R"DOLLAPP(# @dapp-format 1
# @id snake
# @name Snake
# @version 1.0.0
# @boards fnk0104
# @runtime >=1.1.0 <2.0.0
# @summary The classic snake game for the DOLL-OS canvas
# /apps/snake.dapp
# Snake for the DOLL-OS shell. Arrows or WASD to turn, space pauses,
# escape quits. Space or enter on the game over screen plays again.
#
# The snake lives in two places at once: a ring buffer of body cells
# (snx/sny, oldest at $tl, head at $hd) so the tail can be dropped in
# constant time, and an occupancy grid (occ, 0 empty / 1 snake / 2 food)
# so collision and food placement are single lookups instead of a walk
# down the body. Movement is paced off $millis, never off WAIT.

CANVAS 40 22
COLOR green
PUT 14 9 "SNAKE"
COLOR white
PUT 8 11 "arrows or wasd to turn"
PUT 8 12 "space pauses - esc quits"
FLIP
WAIT 1200

# board is 14 x 20 cells, drawn two characters wide so a cell looks square
DIM occ 280
DIM snx 280
DIM sny 280

# best score so far, persisted in /apps/snake.hs across runs
GOSUB loadhs

:newgame
SET score 0
SET stepms 150
SET quit 0
SET paused 0
SET dead 0
SET won 0
SET newhi 0
GOSUB reset
GOSUB placefood
EXPR moveat $millis + $stepms
GOSUB draw

# space both restarts and pauses, so the keypress that started this round has
# to be eaten here or the new game opens paused
:ng_drain
KEY k
IF $k <> 0 GOTO ng_drain

# ---------------------------------------------------------------- main loop

:loop
KEY k
IF $k <> 0 GOSUB handlekey
IF $quit = 1 GOTO leave
IF $paused = 1 GOTO lp_wait
EXPR remain $moveat - $millis
IF $remain > 0 GOTO lp_wait
GOSUB step
IF $dead = 1 GOTO gameover
IF $won = 1 GOTO youwin
EXPR moveat $millis + $stepms
GOSUB draw
:lp_wait
WAIT 12
GOTO loop

# ---------------------------------------------------------------- setup

# three cells long, pointed right, sitting a little left of centre
:reset
SET i 0
:rs_clear
SET occ[$i] 0
ADD i 1
IF $i < 280 GOTO rs_clear
SET tl 0
SET hd 2
SET len 3
SET snx[0] 4
SET sny[0] 10
SET snx[1] 5
SET sny[1] 10
SET snx[2] 6
SET sny[2] 10
SET occ[144] 1
SET occ[145] 1
SET occ[146] 1
SET vx 1
SET vy 0
SET nvx 1
SET nvy 0
RETURN

# random empty cell, with a scan as the fallback once the board is crowded
# enough that guessing stops paying off
:placefood
SET tries 0
:pf_try
RAND fx 14
RAND fy 20
EXPR fi $fy * 14 + $fx
IF $occ[$fi] = 0 GOTO pf_ok
ADD tries 1
IF $tries < 400 GOTO pf_try
SET fi 0
:pf_scan
IF $fi >= 280 GOTO pf_full
IF $occ[$fi] = 0 GOTO pf_found
ADD fi 1
GOTO pf_scan
:pf_found
EXPR fy floor($fi / 14)
EXPR fx $fi - $fy * 14
GOTO pf_ok
:pf_full
SET won 1
RETURN
:pf_ok
SET occ[$fi] 2
RETURN

# ---------------------------------------------------------------- input

:handlekey
IF $k = $kesc GOTO hk_quit
IF $k = $kspace GOTO hk_pause
IF $k = $kleft GOTO hk_left
IF $k = 97 GOTO hk_left
IF $k = $kright GOTO hk_right
IF $k = 100 GOTO hk_right
IF $k = $kup GOTO hk_up
IF $k = 119 GOTO hk_up
IF $k = $kdown GOTO hk_down
IF $k = 115 GOTO hk_down
RETURN

:hk_quit
SET quit 1
RETURN

# Each turn is tested against vx/vy -- the direction already walked -- not
# against the pending one, so mashing up then left inside a single tick
# cannot fold the snake back into its own neck.
:hk_left
IF $vx = 1 GOTO hk_done
SET nvx -1
SET nvy 0
RETURN
:hk_right
IF $vx = -1 GOTO hk_done
SET nvx 1
SET nvy 0
RETURN
:hk_up
IF $vy = 1 GOTO hk_done
SET nvx 0
SET nvy -1
RETURN
:hk_down
IF $vy = -1 GOTO hk_done
SET nvx 0
SET nvy 1
RETURN
:hk_done
RETURN

:hk_pause
IF $paused = 1 GOTO hk_unpause
SET paused 1
GOSUB draw
RETURN
:hk_unpause
SET paused 0
EXPR moveat $millis + $stepms
GOSUB draw
RETURN

# ---------------------------------------------------------------- rules

# one move. Sets dead=1 rather than jumping to the game over screen: a
# GOTO out of a GOSUB would strand its return address on the stack.
:step
SET vx $nvx
SET vy $nvy
EXPR nx $snx[$hd] + $vx
EXPR ny $sny[$hd] + $vy
IF $nx < 0 GOTO st_dead
IF $nx > 13 GOTO st_dead
IF $ny < 0 GOTO st_dead
IF $ny > 19 GOTO st_dead
EXPR ni $ny * 14 + $nx

SET ate 0
IF $occ[$ni] <> 2 GOTO st_tail
SET ate 1
GOTO st_test

# the tail vacates before the head is tested, so chasing the very last
# cell of your own tail is a legal move rather than a death
:st_tail
EXPR ti $sny[$tl] * 14 + $snx[$tl]
SET occ[$ti] 0
ADD tl 1
IF $tl < 280 GOTO st_test
SET tl 0

:st_test
IF $occ[$ni] = 1 GOTO st_dead
ADD hd 1
IF $hd < 280 GOTO st_push
SET hd 0
:st_push
SET snx[$hd] $nx
SET sny[$hd] $ny
SET occ[$ni] 1
IF $ate = 0 GOTO st_done
ADD len 1
ADD score 10
GOSUB speedup
GOSUB placefood
:st_done
RETURN

:st_dead
SET dead 1
RETURN

:speedup
SUB stepms 4
IF $stepms > 55 GOTO su_done
SET stepms 55
:su_done
RETURN

# ---------------------------------------------------------------- drawing

:draw
CLS
COLOR blue
PUT 0 0 "+----------------------------+"
PUT 0 21 "+----------------------------+"
SET dy 1
:dr_side
PUT 0 $dy "|"
PUT 29 $dy "|"
ADD dy 1
IF $dy < 21 GOTO dr_side

SET dy 0
:dr_row
SET dx 0
:dr_col
EXPR ci $dy * 14 + $dx
SET cv $occ[$ci]
IF $cv = 0 GOTO dr_col_next
EXPR scx $dx * 2 + 1
EXPR scy $dy + 1
IF $cv = 2 GOTO dr_food
COLOR green
PUT $scx $scy "[]"
GOTO dr_col_next
:dr_food
COLOR red
PUT $scx $scy "<>"
:dr_col_next
ADD dx 1
IF $dx < 14 GOTO dr_col
ADD dy 1
IF $dy < 20 GOTO dr_row

# the head is just another occupied cell in the grid -- redrawn last, in
# its own colour, so you can tell which end of the snake you are steering
COLOR yellow
EXPR scx $snx[$hd] * 2 + 1
EXPR scy $sny[$hd] + 1
PUT $scx $scy "@@"

COLOR pink
PUT 31 1 "SNAKE"
COLOR cyan
PUT 31 3 "SCORE"
COLOR white
PUT 31 4 $score
COLOR cyan
PUT 31 6 "LEN"
COLOR white
PUT 31 7 $len
COLOR cyan
PUT 31 9 "HI"
COLOR white
PUT 31 10 $hiscore
COLOR yellow
PUT 31 13 "wasd"
PUT 31 14 "or arw"
PUT 31 16 "spc"
PUT 31 17 "pause"
PUT 31 19 "esc"
PUT 31 20 "quit"

IF $paused = 0 GOTO dr_flip
COLOR yellow
PUT 10 10 " PAUSED "
:dr_flip
FLIP
RETURN

# ---------------------------------------------------------------- high score

# reads /apps/snake.hs into hiscore, tolerating a missing or garbled file --
# a fresh install simply starts the ladder at 0
:loadhs
SET hiscore 0
FEXISTS hs "/apps/snake.hs"
IF $hs = 0 GOTO lh_done
FOPEN "/apps/snake.hs" read
IF $fok = 0 GOTO lh_done
FREAD text
FCLOSE
GOSUB str2num
SET hiscore $num
:lh_done
RETURN

:savehs
SET newhi 0
IF $score <= $hiscore GOTO sh_done
SET hiscore $score
SET newhi 1
FOPEN "/apps/snake.hs" write
IF $fok = 0 GOTO sh_done
FWRITE $score
FCLOSE
:sh_done
RETURN

# in: text (string)   out: num -- digits off the front of a string, since a
# number read back from a file is text again
:str2num
SET num 0
SET s2i 0
LEN s2len $text
:s2n_loop
IF $s2i >= $s2len GOTO s2n_done
CHARAT s2d $text $s2i
IF $s2d < 48 GOTO s2n_done
IF $s2d > 57 GOTO s2n_done
SUB s2d 48
MUL num 10
ADD num $s2d
ADD s2i 1
GOTO s2n_loop
:s2n_done
RETURN

# ---------------------------------------------------------------- exits

:gameover
GOSUB savehs
COLOR red
PUT 9 9 " GAME OVER "
GOTO go_banner

:youwin
GOSUB savehs
COLOR green
PUT 4 9 " YOU FILLED THE BOARD "

:go_banner
COLOR white
PUT 8 11 " score $score "
IF $newhi = 0 GOTO go_keys
COLOR yellow
PUT 6 12 " NEW HIGH SCORE "
:go_keys
COLOR cyan
PUT 4 14 "space=again  esc=quit"
FLIP

# swallow whatever was still held down when the snake hit something, so the
# banner is not skipped by the keypress that caused it
WAIT 400
:go_drain
KEY k
IF $k <> 0 GOTO go_drain

:go_wait
KEY k
IF $k = $kspace GOTO newgame
IF $k = $kenter GOTO newgame
IF $k = $kesc GOTO leave
WAIT 20
GOTO go_wait

:leave
ENDCANVAS
COLOR green
PRINT "snake: length $len, score $score (best $hiscore)"
EXIT
)DOLLAPP";

// Generated from apps/dappchat.dapp for firmware-side LittleFS seeding.
static const char BUNDLED_APP_DAPPCHAT[] = R"DOLLAPP(# @dapp-format 1
# @id dappchat
# @name DappChat
# @version 1.1.4
# @boards fnk0104
# @runtime >=1.4.2 <2.0.0
# @echo off
# @summary Chat with other DOLL-OS/DS units over the Sad Girls Club backend
# /apps/dappchat.dapp
# Live network chat: log in (first use of a username creates the account), pick
# a room, then the window refreshes itself on a timer while you type.
#
# The chat screen cannot use INPUT. INPUT blocks until Enter, so the room would
# freeze for exactly as long as you sat composing -- which is most of the time a
# chat is open, and is why this app used to need a blank line to show anything
# new. So the screen is a CANVAS and the input line is edited by hand out of
# KEY, which returns 0 instead of waiting when nobody has typed. That leaves the
# loop free to poll on a clock.
#
# The feed is stored as fixed-width records, $cols characters each, packed into
# one string. That is what makes both operations trivial: rendering row N is a
# SUBSTR at N*$cols, and scrolling is a SUBSTR that drops the first record. A
# separator character would have been simpler to write and impossible to get
# right, since any byte used to join lines is a byte someone can type.
#
# See sgc-chat-server/README.md for the backend this talks to.

SETSTR endpoint "https://sadgirlsclub.wtf/dappchat"
SETSTR room "lobby"
SET since 0
CHR q 34

# A padding space has to come from CHR, never from a " " literal. The shell
# splitter removes an argument's quotes as it splits, and the runtime's expander
# then trims what it is handed -- so on hardware a quoted whitespace-only literal
# arrives as the empty string and APPEND adds nothing, silently. (The browser
# runtime strips quotes later, so the same line works there, which is exactly how
# this shipped.) A $-reference is read straight out of the variable and never
# trimmed, so it survives both.
CHR sp 32

# Canvas geometry: header, feed, status, prompt. The panel scales its glyphs to
# fit whatever grid it is handed, so these numbers are the font size -- fewer
# columns means bigger text. 40x15 reads comfortably; raise $cols and $rows
# together for more history at a smaller size.
SET cols 40
SET rows 15
SET feedrows 11
SET statusrow 12
SET promptrow 14

# how long the room may go unrefreshed, in milliseconds
SET pollms 2000

COLOR pink
PRINT "DAPPCHAT"
PRINT "letters/numbers/-/_ only for username and room"
COLOR white

HTTPCLEAR
HTTPHEADER "Content-Type" "application/json"

:login
INPUT username "username> "
IFEQ $username "" GOTO login
INPUTSECRET password "password> "

JSONESC safeuser $username
JSONESC safepass $password
SETSTR password ""

SETSTR sbody "{"
APPEND sbody $q
APPEND sbody "username$q:"
APPEND sbody $q
APPEND sbody "$safeuser$q,"
APPEND sbody $q
APPEND sbody "password$q:"
APPEND sbody $q
APPEND sbody "$safepass$q}"
SETSTR safepass ""

SETSTR url "$endpoint/auth"
PRINT "signing in..."
HTTPPOST raw $url $sbody 512
SETSTR sbody ""
IF $httpok = 0 GOTO auth_http_error

JSONGET text $raw "ok"
IFEQ $text "true" GOTO auth_ok
JSONGET text $raw "error"
COLOR red
IFEQ $text "bad_password" GOTO bad_password
PRINT "sign-in failed: HTTP $httpcode ($httplen bytes)"
PRINT "$raw"
COLOR white
GOTO login

:bad_password
PRINT "wrong password for $username"
COLOR white
GOTO login

:auth_http_error
COLOR red
PRINT "could not reach dappchat: HTTP $httpcode"
COLOR white
WAIT 1500
GOTO login

:auth_ok
JSONGET authtoken $raw "token"
JSONGET text $raw "created"
COLOR green
IFEQ $text "true" GOTO welcome_new
PRINT "welcome back, $username"
GOTO room_prompt
:welcome_new
PRINT "account created - welcome, $username"

:room_prompt
COLOR white
INPUT line "room [$room]> "
IFEQ $line "" GOTO room_default
SETSTR room $line
:room_default
SET since 0

HTTPHEADER "Authorization" "Bearer $authtoken"

SETSTR feed ""
SET feedlines 0
SETSTR line ""
SETSTR status "joining $room..."
SET dirty 1
SET nextpoll 0

CANVAS $cols $rows

# ------------------------------------------------------------- the chat loop

# One pass: refresh if the clock says so, drain whatever was typed, redraw if
# anything changed, then hand the CPU back. WAIT is also what clears the
# runaway-loop step counter, so it has to stay on this path.
:loop
IF $millis < $nextpoll GOTO loop_keys
GOSUB poll
SET nextpoll $millis
ADD nextpoll $pollms

# Drain the whole key queue each pass rather than one key per frame, so fast
# typing cannot fall behind the redraw.
:loop_keys
KEY k
IF $k = 0 GOTO loop_draw
IF $k = $kesc GOTO done
IF $k = $kenter GOTO loop_enter
# The runtime folds CR and LF into $kenter before a script sees them, but a
# keyboard bridge that ever hands one through raw should still send the line
# rather than silently swallowing the keystroke.
IF $k = 13 GOTO loop_enter
IF $k = 10 GOTO loop_enter
IF $k = $kback GOTO loop_back
IF $k = 127 GOTO loop_back
IF $k < 32 GOTO loop_unused
IF $k > 126 GOTO loop_unused
LEN llen $line
IF $llen >= 200 GOTO loop_keys
CHR kc $k
APPEND line $kc
SET dirty 1
GOTO loop_keys

# Naming the code makes a keyboard that disagrees with the runtime diagnosable
# from the screen instead of by guesswork: if Enter arrives as something this
# app does not act on, the status row says which number it was.
:loop_unused
SETSTR status "unused key ($k)"
SET dirty 1
GOTO loop_keys

:loop_back
LEN llen $line
IF $llen = 0 GOTO loop_keys
SUB llen 1
SUBSTR line $line 0 $llen
SET dirty 1
GOTO loop_keys

:loop_enter
IFEQ $line "" GOTO loop_now
IFEQ $line "/quit" GOTO done
GOSUB send
SETSTR line ""
SET dirty 1
GOTO loop_keys

# Enter on an empty line still means "refresh right now" -- the timer is the
# normal path, but there is no reason to make someone wait it out.
:loop_now
SET nextpoll 0
GOTO loop_keys

:loop_draw
IF $dirty = 1 GOSUB render
WAIT 20
GOTO loop

# ---------------------------------------------------------------- the screen

:render
SET dirty 0
CLS
COLOR cyan
SETSTR padtext "$room - enter sends, esc quits"
GOSUB padline
PUT 0 0 $padtext
COLOR white

SET rrow 1
SET roff 0
SET rleft $feedlines
:render_row
IF $rleft = 0 GOTO render_status
SUBSTR padtext $feed $roff $cols
PUT 0 $rrow $padtext
ADD roff $cols
ADD rrow 1
SUB rleft 1
GOTO render_row

:render_status
IFEQ $status "" GOTO render_prompt
COLOR yellow
SETSTR padtext $status
GOSUB padline
PUT 0 $statusrow $padtext
COLOR white

:render_prompt
SETSTR padtext "$username> $line"
LEN plen $padtext
IF $plen <= $cols GOTO render_prompt_pad
# A narrow grid runs out of room long before the 200 characters the server will
# take, so keep the end of what is being typed on screen rather than the start.
SET ptail $plen
SUB ptail $cols
SET plen $cols
SUBSTR padtext $padtext $ptail $plen
:render_prompt_pad
GOSUB padline
PUT 0 $promptrow $padtext
FLIP
RETURN

# in/out: padtext -- exactly $cols wide, so every PUT overwrites the whole row
# and no stale text survives a redraw
:padline
LEN plen $padtext
IF $plen > $cols GOTO pad_cut
:pad_grow
IF $plen >= $cols GOTO pad_done
APPEND padtext $sp
ADD plen 1
GOTO pad_grow
:pad_cut
SUBSTR padtext $padtext 0 $cols
:pad_done
RETURN

# in: padtext (raw line) -- appends it to the feed, wrapping anything wider than
# the grid across as many records as it needs. The server takes 200 characters
# and a readable grid is 40 wide, so clipping would routinely eat most of a
# message rather than the odd long one.
:feed_add
SETSTR faline $padtext
LEN falen $faline
IF $falen = 0 GOTO feed_done
SET faoff 0
:feed_chunk
SET facount $falen
SUB facount $faoff
IF $facount <= $cols GOTO feed_chunk_take
SET facount $cols
:feed_chunk_take
SUBSTR padtext $faline $faoff $facount
GOSUB padline
APPEND feed $padtext
ADD feedlines 1
ADD faoff $facount
GOSUB feed_scroll
IF $faoff < $falen GOTO feed_chunk
:feed_done
RETURN

# drops whole records off the top until the window fits again
:feed_scroll
IF $feedlines <= $feedrows GOTO feed_scroll_done
LEN flen $feed
SUB flen $cols
SUBSTR feed $feed $cols $flen
SUB feedlines 1
GOTO feed_scroll
:feed_scroll_done
RETURN

# ---------------------------------------------------------------- networking

# Appends anything newer than $since to the feed and advances $since. The server
# answers at most 10 messages per request and since=0 means "the 10 most recent",
# so a response that comes back full means more is waiting; $pround caps the
# catch-up at 50 rather than replaying a whole room.
:poll
SET pround 0
:poll_round
SETSTR url "$endpoint/poll?room=$room&since=$since"
HTTPGET praw $url 4096
IF $httpok = 1 GOTO poll_body
# The refresh after a pause is the request most likely to land on a keep-alive
# socket the server has already closed. That attempt is lost; the retry gets a
# fresh connection.
HTTPGET praw $url 4096
IF $httpok = 0 GOTO poll_failed
:poll_body
SET pi 0
:poll_next
JSONGET ptext $praw "messages[$pi].text"
IF $jsonok = 0 GOTO poll_done
JSONGET puser $praw "messages[$pi].user"
SETSTR padtext "$puser: $ptext"
GOSUB feed_add
SET dirty 1
ADD pi 1
GOTO poll_next
:poll_done
SETSTR status ""
JSONGET text $praw "last_id"
IF $jsonok = 0 GOTO poll_return
GOSUB str2num
SET since $num
IF $pi < 10 GOTO poll_return
ADD pround 1
IF $pround < 5 GOTO poll_round
:poll_return
RETURN

:poll_failed
SETSTR status "refresh failed (HTTP $httpcode) - still trying"
SET dirty 1
RETURN

# in: line
:send
JSONESC safemsg $line
IF $jsonok = 0 GOTO send_too_long

SETSTR sbody "{"
APPEND sbody $q
APPEND sbody "room$q:"
APPEND sbody $q
APPEND sbody "$room$q,"
APPEND sbody $q
APPEND sbody "text$q:"
APPEND sbody $q
APPEND sbody $safemsg
APPEND sbody $q
APPEND sbody "}"

SETSTR url "$endpoint/send"
HTTPPOST sraw $url $sbody 512
IF $httpok = 0 GOTO send_http_error

# Every endpoint answers HTTP 200 and reports success inside the body, so $httpok
# is 1 for a refusal too. A send the server rejected has to be read out of "ok"
# or the message simply evaporates: nothing on screen, nothing in the room, and
# nothing for the next poll to return because the text was never stored.
JSONGET sendok $sraw "ok"
IFEQ $sendok "true" GOTO send_ok
JSONGET senderr $sraw "error"
IFEQ $senderr "unauthorized" GOTO send_unauthorized
SETSTR status "not sent ($senderr)"
SET dirty 1
RETURN

:send_ok
# show it now rather than at the end of the poll interval
SET nextpoll 0
SETSTR status ""
RETURN

# The server keeps one token per account and overwrites it on every sign-in, so
# signing the same username in on a second unit silently retires this one's.
# Reading a room needs no token at all, which is what makes this look like a
# chat that receives fine and drops everything you type.
:send_unauthorized
SETSTR status "signed out - is $username signed in on another unit?"
SET dirty 1
RETURN

:send_http_error
SETSTR status "send failed (HTTP $httpcode)"
SET dirty 1
RETURN

:send_too_long
SETSTR status "message too long after JSON escaping"
SET dirty 1
RETURN

# in: text (string)   out: num -- digits off the front of a string, since a
# JSON number comes back through JSONGET as text (the tetris/snake routine)
:str2num
SET num 0
SET s2i 0
LEN s2len $text
:s2n_loop
IF $s2i >= $s2len GOTO s2n_done
CHARAT s2d $text $s2i
IF $s2d < 48 GOTO s2n_done
IF $s2d > 57 GOTO s2n_done
SUB s2d 48
MUL num 10
ADD num $s2d
ADD s2i 1
GOTO s2n_loop
:s2n_done
RETURN

:done
ENDCANVAS
HTTPCLEAR
SETSTR authtoken ""
COLOR pink
PRINT "bye"
EXIT
)DOLLAPP";

// Generated from apps/dappstore.dapp for firmware-side LittleFS seeding.
static const char BUNDLED_APP_DAPPSTORE[] = R"DOLLAPP(# @dapp-format 1
# @id dappstore
# @name Dappstore
# @version 1.0.0
# @boards fnk0104
# @runtime >=1.6.0 <2.0.0
# @echo off
# @summary Browse, install, update and repair verified Dapper packages
# /system/apps/dappstore.dapp
#
# A guided front end for the runtime's deliberately narrow DAPPER bridge. The
# bridge enters Dapper.ino directly rather than executing a shell command, so
# repository identity checks, HTTPS verification, SHA-256 validation, atomic
# replacement, managed-path rules and rollback remain exactly the same as at
# the shell. This app adds prompts and confirmations; it does not install files
# itself.

:menu
CLEAR
COLOR pink
PRINT "========================================"
PRINT "              DAPPSTORE"
PRINT "========================================"
COLOR white
PRINT "1  browse compatible packages"
PRINT "2  search the catalog"
PRINT "3  package details"
PRINT "4  install a package"
PRINT "5  update packages"
PRINT "6  remove a package"
PRINT "7  refresh catalog"
PRINT "8  verify installed packages"
PRINT "q  return to DOLL-OS"
PRINT ""
INPUT choice "store> "
IFEQ $choice "1" GOTO browse
IFEQ $choice "2" GOTO search
IFEQ $choice "3" GOTO info
IFEQ $choice "4" GOTO install
IFEQ $choice "5" GOTO update
IFEQ $choice "6" GOTO remove
IFEQ $choice "7" GOTO refresh
IFEQ $choice "8" GOTO doctor
IFEQ $choice "q" GOTO leave
IFEQ $choice "Q" GOTO leave
GOTO menu

:browse
CLEAR
COLOR cyan
PRINT "COMPATIBLE PACKAGES"
COLOR white
DAPPER search
GOSUB pause
GOTO menu

:search
CLEAR
INPUT query "search> "
LEN n $query
IF $n = 0 GOTO menu
COLOR cyan
PRINT "RESULTS FOR: $query"
COLOR white
DAPPER search $query
GOSUB pause
GOTO menu

:info
CLEAR
INPUT id "package id> "
LEN n $id
IF $n = 0 GOTO menu
DAPPER info $id
GOSUB pause
GOTO menu

:install
CLEAR
COLOR cyan
PRINT "INSTALL PACKAGE"
COLOR white
INPUT id "package id> "
LEN n $id
IF $n = 0 GOTO menu
DAPPER info $id
PRINT ""
INPUT place "internal flash or SD [i/s]> "
INPUT confirm "type INSTALL to continue> "
IFNE $confirm "INSTALL" GOTO cancelled
IFEQ $place "s" GOTO installsd
IFEQ $place "S" GOTO installsd
IFEQ $place "sd" GOTO installsd
IFEQ $place "SD" GOTO installsd
DAPPER install $id --internal
GOSUB pause
GOTO menu
:installsd
DAPPER install $id --sd
GOSUB pause
GOTO menu

:update
CLEAR
COLOR cyan
PRINT "UPDATE PACKAGES"
COLOR white
PRINT "Leave the ID empty to update every managed package."
INPUT id "package id> "
INPUT confirm "type UPDATE to continue> "
IFNE $confirm "UPDATE" GOTO cancelled
LEN n $id
IF $n = 0 GOTO updateall
DAPPER update $id
GOSUB pause
GOTO menu
:updateall
DAPPER update --all
GOSUB pause
GOTO menu

:remove
CLEAR
COLOR yellow
PRINT "REMOVE PACKAGE"
COLOR white
PRINT "The app is removed; its save data is kept."
INPUT id "package id> "
LEN n $id
IF $n = 0 GOTO menu
DAPPER info $id
INPUT confirm "type REMOVE to continue> "
IFNE $confirm "REMOVE" GOTO cancelled
DAPPER remove $id
GOSUB pause
GOTO menu

:refresh
CLEAR
COLOR cyan
PRINT "REFRESH CATALOG"
COLOR white
DAPPER refresh
GOSUB pause
GOTO menu

:doctor
CLEAR
COLOR cyan
PRINT "VERIFY INSTALLED PACKAGES"
COLOR white
DAPPER doctor
GOSUB pause
GOTO menu

:cancelled
COLOR yellow
PRINT "cancelled"
GOSUB pause
GOTO menu

:pause
COLOR cyan
PRINT ""
INPUT waitkey "press enter> "
RETURN

:leave
COLOR pink
PRINT "Dappstore closed"
EXIT
)DOLLAPP";

// Generated from docs/DAPP.txt for firmware-side LittleFS seeding.
static const char BUNDLED_DOC_DAPP[] = R"DOLLDOC(DOLL-OS .dapp Apps

.dapp files are text executables for the DOLL-OS shell. DOLL-OS looks for them in both
places:

/sd/apps   on the SD card (upload with FTP into /apps)
/apps      on internal flash (LittleFS on the SPIFFS-labeled partition)

Normal sketch upload and flash upload are separate on this board:

Upload          flashes firmware only
LittleFS/SPIFFS uploads the filesystem image

So data/apps/<name>.dapp only lands on the device when you run a dedicated
filesystem upload. Built-in firmware-seeded apps are written to /system/apps
on boot; /sd/apps and /apps override a built-in app with the same name.

Use:

apps
run hello
run /apps/hello.dapp
run /sd/apps/hello.dapp

Example:

# /sd/apps/hello.dapp
COLOR cyan
PRINT "hello from a DOLL-OS app"
PRINT "cwd=$cwd ip=$ip battery=$battery%"
INPUT name "name> "
PRINT "hi, $name"
RAND lucky 1 100
PRINT "lucky number: $lucky"
WAIT 750
COLOR pink
PRINT "tiny executable acquired"
EXIT

Editing:

edit /sd/apps/hello.dapp syntax highlights the file on both the panel and a
connected telnet client. Comments are grey, commands cyan, labels pink, quoted
strings green, $variables yellow, and numbers orange. Highlighting keys off the
.dapp extension, so saving under a new name with ^O switches it on or off.
An opcode that stays white is one run will reject as unknown.

edit --repo snake downloads the compatible repository version into the editor
without installing it first. ^O writes it to the suggested app path, or to any
path you type into the save prompt.

Metadata directives:

Package metadata lives in leading comments such as # @id hello. AppRunner also
honors # @echo off, which keeps INPUT and INPUTSECRET from copying the submitted
prompt line into scrollback. Use it for chat-style apps that print the submitted
text later through their own display flow. The default is echo on.

Commands:

PRINT <text>         print text, with $variables expanded
ECHO <text>          alias for PRINT
COLOR <name>         white, red, green, yellow, blue, magenta, cyan, pink
CLEAR                clear the terminal, or the canvas if one is up
CLS                  alias for CLEAR
WAIT <ms>            pause while keeping display/radio/FTP serviced
SLEEP <ms>           alias for WAIT
SET <name> <value>   set a numeric variable
ADD <name> <value>   add to a numeric variable
SUB <name> <value>   subtract from a numeric variable
MUL <name> <value>   multiply a numeric variable
DIV <name> <value>   divide a numeric variable (integer result)
MOD <name> <value>   remainder of a numeric variable
EXPR <name> <expr>   evaluate a full arithmetic expression
RAND <n> <max>       set numeric variable n to 0..max-1
RAND <n> <min> <max> set numeric variable n to min..max
DIM <name> <size>    create a numeric array of size cells, all zero
LIFE <cur> <next> <cols> <rows>  advance Conway's Life in numeric arrays
SETSTR <name> <txt>  set a string variable
APPEND <name> <txt>  append to a string variable
CHR <name> <code>    set a string variable to one character by code
HEX <name> <value> [width]  uppercase hexadecimal text, width 1..8
SUBSTR <n> <t> <s> <c>  slice a string into a string variable
LEN <name> <text>    character count into a numeric variable
CHARAT <n> <t> <i>   character code at index i, or 0 past the end
INPUT <name> [p]     read a line into a string variable (blocks until Enter)
INPUTSECRET <name> [p] read a masked line into a string variable
KEY <name>           read one keypress into a numeric variable, 0 if none
WAVE <ch> <kind> <hz> <level>  set sine/triangle/square/sawtooth/noise/off channel 1..3
WAVESTOP             silence the synth and release the audio hardware
HTTPGET <n> <url> [max] bounded HTTP/HTTPS GET into string variable n
HTTPPOST <n> <url> <body> [max] bounded POST response into string variable n
HTTPHEADER <name> <value> set/replace one of up to eight request headers
HTTPCLEAR             clear all request headers
HTTPGETBUF <url> [max] GET into the byte buffer, past the 4096-byte string cap
BUFNEW [bytes]        allocate the byte buffer (default 65536, maximum 262144)
BUFFREE / BUFCLEAR    release the buffer / empty it without reallocating
BUFAT <n> <pos>       byte at pos into numeric variable n, 0 past the end
BUFSUB <n> <pos> <ct> copy a slice of the buffer into string variable n
BUFWRITE <pos> <text> copy text into the buffer at pos
BUFSCAN <n> <pos> [stops] advance past bytes until one in stops ("" = space)
BUFTAKE <s> <n> <pos> [stops] same, and copy what was passed into string s
BUFSAVE <path>        write the buffer to a file; BUFLOAD reads one back
HTMLTEXT <url> <textpath> <linkpath|-> [wrap] [maxlinks] fetch and render a page
HTMLOPEN <textpath> <linkpath|-> <base> [wrap] [maxlinks] start a render
HTMLFEED url <url> | buf [pos ct] | text <s>  add a source to the open render
HTMLCLOSE             finish the render and close its files
HTMLSTR <n> url <url> | buf | text <s>  render into string variable n
URLABS <n> <base> <href> absolute URL into n, or "" if it is not followable
URLPART <n> <url> scheme|origin|host|path|dir  one piece of a URL
DAPPER <action> [args] call the verified Dapper package manager
JSONESC <n> <text>    escape text for insertion inside a JSON string
JSONGET <n> <json> <path> extract a JSON value; $jsonok reports success
FOPEN <path> <mode>  open a file: read, write, append, or update
FCLOSE               close it (automatic when the app ends)
FREAD <name>         read one line into a string variable; $feof goes 1 at end
FREADB <name>        read one raw byte (0..255); $feof distinguishes NUL/EOF
FWRITE <text>        write a line, with $variables expanded
FWRITEB <value>      write one raw byte (0..255)
FSEEK <offset>       move to an absolute byte offset; $fok reports success
FTELL <name>         current byte offset into a numeric variable
FSIZE <name>         open file size into a numeric variable
FEXISTS <n> <path>   1 into numeric variable n if the path exists
FDELETE <path>       delete a file; $fok reports success
FLIST <dir> <output> snapshot children as D|name|0 or F|name|bytes
FMKDIR <path>        create one directory; $fok reports success
FCOPY <src> <dst>    copy one file without overwriting
FMOVE <src> <dst>    move one file without overwriting
CANVAS <cols> <rows> switch the display to a character grid
ENDCANVAS            leave canvas mode, restoring the terminal
PUT <col> <row> <t>  draw text into the canvas in the current COLOR
FLIP                 push the canvas to the panel and telnet client
LABEL <name>         define a jump target
:<name>              shorthand label
GOTO <name>          jump to a label
GOSUB <name>         call a label, returning to the next line
RETURN               return from the most recent GOSUB
IF <l> <op> <r> GOTO|GOSUB <name>
IFEQ <l> <r> GOTO|GOSUB <name>
IFNE <l> <r> GOTO|GOSUB <name>
EXIT                 leave the app
END                  alias for EXIT

IF supports =, ==, !=, <>, <, <=, >, and >=.
RAND roll 6 returns 0..5; RAND roll 1 6 returns 1..6.
IFEQ and IFNE compare strings. Quote string literals that contain spaces.

Arrays:

DIM makes a numeric array. Cells are read as $name[index] anywhere a value is
accepted, and written by using name[index] where a variable name goes:

DIM well 200
SET well[0] 3
EXPR i $y * 10 + $x
SET well[$i] $well[0]
IF $well[$i] <> 0 GOTO occupied

The index is itself a value, so $board[$row] and $board[$a[1]] both work. An
index outside the array stops the app with the offending line number rather
than quietly reading zero. All arrays share a pool of 8192 cells.

LIFE cur nxt 38 20 is a native Conway's Game of Life step for dense grids.
Both arrays must be DIM'd with at least cols * rows cells. It reads cur, writes
the next generation through nxt, then copies the result back into cur.

Arithmetic:

SET/ADD/SUB/MUL/DIV/MOD each take one value, which is enough for counters and
awkward for anything else. EXPR takes a whole expression instead and hands it
to the same evaluator the calc command uses:

EXPR index $row * 10 + $col
EXPR wrapped ($angle + 360) % 360
EXPR level floor($lines / 10) + 1

+ - * / ^ % work, along with abs, floor, ceil, sqrt, pow, and the trig
functions -- the calc help list. Spaces are fine. Every $name is replaced by
its numeric value before evaluation, and the result is rounded to a whole
number on the way into the variable. Note that / divides as a real number:
EXPR a $b / 10 on b = 5 stores 1, not 0. Wrap it in floor() to truncate.

Subroutines:

GOSUB jumps like GOTO but remembers where it came from, and RETURN goes back to
the line after the call. Nesting is allowed up to 64 deep.

GOSUB redraw
GOTO done

:redraw
PUT 0 0 "score $score"
FLIP
RETURN

One rule is worth stating plainly: a routine must leave through RETURN, not by
GOTO-ing somewhere else. Jumping out strands the return address on the stack,
and enough of those in a loop will hit the depth limit. If a routine needs to
end the round, set a variable and let the caller act on it.

Keys and games:

INPUT blocks until Enter, which makes it useless for anything that has to keep
moving while nobody is typing. KEY reads at most one keypress and returns
immediately with 0 when nothing is waiting, from either the telnet client or a
BLE keyboard on the companion DOLL-OS keyboard bridge:

:loop
KEY k
IF $k = $kleft GOSUB move_left
IF $k = $kesc GOTO quit
WAIT 16
GOTO loop

Printable keys come back as their ASCII code (65 for A, 32 for space), and
these built-ins name the rest: $kup, $kdown, $kleft, $kright, $kenter, $kesc,
$kback, $ktab, $kspace. They are numbers, not text -- compare against them,
don't PRINT them. Ctrl+C and Ctrl+T both read as $kesc, so the chord that
leaves every other DOLL-OS screen also leaves yours. Ctrl+X is never delivered to
the script: it aborts the running app, from KEY, WAIT, or a blocked INPUT.

CANVAS replaces the scrolling terminal with a grid you address by cell. PUT
writes into the grid without drawing anything, and FLIP shows the result -- so
a frame is assembled off-screen and appears at once. On the panel the grid is
scaled up to fill the terminal area, so a 10x20 playfield gets big cells and an
80x24 status screen gets small ones. CLS blanks the grid while a canvas is up.
ENDCANVAS puts the terminal back, and so does leaving the app.

CANVAS 20 10
COLOR cyan
PUT 6 4 "hello"
FLIP
WAIT 1000
ENDCANVAS

A canvas is at most 120 by 60 cells. run tetris is the worked example: a well
in a 200-cell array, pieces rotated with EXPR, gravity paced off $millis, and
every frame drawn cell by cell.

Files:

One file can be open at a time -- FOPEN closes any previous one, and the app
ending closes the last. Paths work exactly like shell paths: /sd/... is the
card, everything else is flash, relative paths resolve against the shell's
cwd, and $variables expand inside them.

FOPEN "/apps/scores.txt" append
FWRITE "$name $score"
FCLOSE

Reading is line by line; $feof becomes 1 when a read finds nothing left (an
empty line mid-file leaves it 0, so the two are distinguishable):

FOPEN "/apps/scores.txt" read
:rl
FREAD line
IF $feof = 1 GOTO done
PRINT $line
GOTO rl
:done
FCLOSE

A file that can't be opened is not an error -- $fok is 0 and the script
decides, because a missing save file is a normal situation. Misusing the
handle is an error: FREAD with nothing open (or a file opened for write), or
FWRITE on a file opened for read, stops the app like any other bug. Numbers
written with FWRITE come back as text -- parse digits with CHARAT before
doing math on them. run tetris does exactly this for its persistent high
score in /apps/tetris.hs.

For binary files, open with update (or rw/r+), then use FREADB/FWRITEB and
FSEEK. FREADB returns every byte as 0..255, so 0x00 and 0x0A are never eaten by
line handling; $feof alone means no byte was available. FTELL and FSIZE expose
positions, and HEX formats a value as 1..8 uppercase hexadecimal digits. run
hex is a complete byte-safe editor built on these operations.

Audio and HTTP:

WAVE controls three mixed PCM channels on the onboard speaker. Kinds are sine,
triangle, square, sawtooth, noise, and off; frequency is 1..12000Hz and level
is 0..100. sawtooth requires AppRunner >=1.8.0. WAVESTOP (and app exit)
silences/releases the hardware. $audiook reports the most recent start. run
synth is the interactive mixer and waveform display.

HTTPGET body "https://example.com/data.json" 2048 stores a bounded text body.
HTTPPOST adds a request body; HTTPHEADER/HTTPCLEAR manage up to eight headers.
$httpok, $httpcode, $httplen, and $httptruncated report the result. HTTPS is
encrypted but generic script URLs are not certificate-authenticated; Dapper's
verified package-download path is separate. Up to five redirects are followed.
Successive requests to the same origin reuse one connection, so a script that
fetches repeatedly from a host pays for the TLS handshake once; the connection
is dropped when the origin changes, on a truncated response, and at app exit.

Buffers:

A string variable caps at 4096 bytes, so HTTPGET cannot hold a document. BUFNEW
allocates one flat block of up to 262144 raw bytes and HTTPGETBUF fills it from
the network. $buflen and $bufcap report the fill and the size.

BUFAT reads one byte and BUFSUB copies a slice, but walking a document one BUFAT
at a time costs an interpreter step per byte. BUFSCAN and BUFTAKE stop on a
character class instead, so a scan costs a step per token rather than per byte.
An empty stop set means whitespace. BUFSAVE/BUFLOAD move the buffer to and from
a file and need the script's own file handle closed. The buffer is released when
the app exits.

HTML:

HTMLTEXT "https://example.com/" "/page.txt" "/page.lnk" 76 200 fetches a page and
renders it to readable text in one instruction. The body is streamed through the
renderer as it arrives, so the page is never held anywhere and its size is capped
by the filesystem rather than by any runtime limit.

/page.txt holds word-wrapped text with each link marked [n] in place; /page.lnk
holds one "<n> <absolute-url>" line per link in marker order, so link n is line n.
Pass - as the link path to skip link collection. $htmllines, $htmllinks,
$htmlbytes, and $htmlok report the render.

HTMLOPEN/HTMLFEED/HTMLCLOSE are the same renderer with the pieces exposed, for
rendering several sources into one file or for HTML the script already holds.
HTMLSTR renders into a string variable when no file is wanted.

Relative links are resolved against the page's own URL, so the link file is
directly fetchable. URLABS and URLPART expose that resolution on its own; URLABS
returns "" for anything not followable over http(s).

The renderer skips <script>, <style>, and comments, understands entities and
UTF-8, and transliterates non-ASCII to ASCII because the panel font draws nothing
above 126. No JS, CSS, images, or forms. It opens two files of its own and the
runtime allows the script only one, so HTMLOPEN and HTMLTEXT refuse to start
while a script file is open.

Dapper bridge:

DAPPER calls only the built-in Dapper package manager; it is not a general shell
escape. Actions are search, info, install, update, remove, refresh, doctor, and
runtime. For example: DAPPER install snake --internal. Repository identity,
compatibility, HTTPS, SHA-256, managed-path, atomic replacement, and rollback
checks are the same ones used by the shell command. Confirm remove operations in
the app, and pass --internal or --sd to avoid Dapper's location prompt.

JSONESC safely quotes user text before it is placed inside JSON. JSONGET walks
paths such as choices[0].message.content. $jsonok reports malformed JSON,
missing paths, or an escaped/extracted value that exceeds the string limit.

Time:

TIME (AppRunner >=1.9.0) syncs the clock over NTP and reports the result as
UTC -- there is no timezone setting, so a script wanting local time adds its
own offset. $timeok is 1 on success, 0 if there is no network or NTP did not
answer within about 8 seconds; check it before trusting the rest. $timeepoch
is Unix seconds; $timeyear, $timemonth (1-12), $timeday, $timehour,
$timeminute, $timesecond, and $timeweekday (0=Sunday..6=Saturday) are the same
moment already split into fields, since there is no calendar math in EXPR to
do it yourself. The device caches a synced clock, so a TIME call after the
first one in a session usually returns immediately.

Limits:

A script is read into RAM in full before its first line runs. The storage comes
from PSRAM, so the caps are roomy:

4000       lines per app (same cap as the edit editor)
512        labels
128        numeric variables
64         string variables
32         arrays, sharing a pool of 16384 cells
64         nested GOSUB calls
1          open file at a time
120 x 60   largest canvas
4096       characters per string variable
262144     bytes in the byte buffer (BUFNEW), default 65536
1000000    executed steps between waits before the loop guard trips

HTMLTEXT is not on that list: it streams to disk and never holds the page.

Hitting one is reported on the terminal rather than failing silently. The step
guard counts instructions since the last WAIT or INPUT, on the grounds that a
runaway loop is precisely one that never yields -- so a game pacing itself with
WAIT 16 can run all day, while a bare GOTO loop still trips. A loop that WAITs
forever, on purpose or by accident, is stopped from the keyboard instead:
Ctrl+X aborts any running app.

Built-ins usable as $name or numeric values:

$battery
$cwd
$heap
$ip
$millis
$seconds
$wifi
$ledok
$audiook
$httpok
$httpcode
$httplen
$httptruncated
$jsonok
$buflen
$bufcap
$bufok
$htmlok
$htmllines
$htmllinks
$htmlbytes

Numeric only (see Keys and games): $kup, $kdown, $kleft, $kright, $kenter,
$kesc, $kback, $ktab, $kspace; plus the file-op status pair $fok (last
FOPEN/FDELETE succeeded) and $feof (last FREAD hit end of file).

Interactive Example:

# /sd/apps/ask.dapp
COLOR pink
PRINT "tiny prompt"

:again
INPUT reply "say> "
IFEQ $reply "/quit" GOTO done
PRINT "you said: $reply"
GOTO again

:done
PRINT "bye"
EXIT
)DOLLDOC";
