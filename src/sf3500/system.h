#ifndef SF3500_SYSTEM_H
#define SF3500_SYSTEM_H

extern char quitGame;

int PHL_MainLoop();
void PHL_ConsoleInit();
void PHL_GameQuit();

void PHL_ErrorScreen(char* message);

#endif
