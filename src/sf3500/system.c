#include "system.h"

#include <stdio.h>

char quitGame = 0;

int PHL_MainLoop()
{
	return !quitGame;
}

void PHL_ConsoleInit()
{
}

void PHL_GameQuit()
{
	quitGame = 1;
}

void PHL_ErrorScreen(char* message)
{
	fprintf(stderr, "%s\n", message);
}
