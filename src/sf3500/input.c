#include "input.h"
#include "joykey.h"
#include "hwdisp.h"
#include "hwaudio.h"

#include <stdlib.h>

Button btnUp = {0}, btnDown = {0}, btnLeft = {0}, btnRight = {0};
Button btnFaceUp = {0}, btnFaceDown = {0}, btnFaceLeft = {0}, btnFaceRight = {0};
Button btnL = {0}, btnR = {0};
Button btnStart = {0}, btnSelect = {0};
Button btnAccept = {0}, btnDecline = {0};
int axisX = 0, axisY = 0;
int useJoystick = 1;

static void updateKey(Button* btn, int state)
{
	if (state) {
		btn->pressed = (btn->held == 1) ? 0 : 1;
		btn->held = 1;
		btn->released = 0;
	} else {
		btn->released = (btn->held == 1) ? 1 : 0;
		btn->held = 0;
		btn->pressed = 0;
	}
}

void PHL_ScanInput()
{
	unsigned int k = joykey_read();

	/* Hard exit bypass - SELECT+START together, independent of whatever the
	 * game's own quit menu does. Standing safety net for every port on this
	 * device: a game's own menu path has previously hung/crashed on real
	 * hardware (Manic Miner), and this one hasn't been tested standalone yet. */
	if ((k & (1u << JK_SELECT)) && (k & (1u << JK_START))) {
		hwaudio_deinit();
		hwdisp_deinit();
		exit(0);
	}

	updateKey(&btnUp,    (k >> JK_UP)    & 1);
	updateKey(&btnDown,  (k >> JK_DOWN)  & 1);
	updateKey(&btnLeft,  (k >> JK_LEFT)  & 1);
	updateKey(&btnRight, (k >> JK_RIGHT) & 1);

	updateKey(&btnStart,  (k >> JK_START)  & 1);
	updateKey(&btnSelect, (k >> JK_SELECT) & 1);

	updateKey(&btnL, (k >> JK_L1) & 1);
	updateKey(&btnR, (k >> JK_R1) & 1);

	/* A=jump, B=attack, X=secondary weapon, Y=unused-by-game "any key" slot -
	 * same scheme as upstream's own BITTBOY keyboard mapping (A - jump,
	 * B - slash). Accept/Decline reuse the attack/jump buttons, matching
	 * every other backend's own convention (see src/sdl/input.c). */
	updateKey(&btnFaceDown,  (k >> JK_A) & 1);
	updateKey(&btnFaceLeft,  (k >> JK_B) & 1);
	updateKey(&btnFaceRight, (k >> JK_X) & 1);
	updateKey(&btnFaceUp,    (k >> JK_Y) & 1);

	updateKey(&btnAccept,  (k >> JK_B) & 1);
	updateKey(&btnDecline, (k >> JK_A) & 1);
}
