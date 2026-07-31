#pragma once

// Chat commands ("!rank", "/rank", ...).
//
// The say hook runs *before* the engine broadcasts the player's own line, so
// running a command inline would print its output above the "!session" the
// player just typed. Instead we recognise it in the hook and run it one frame
// later, after the chat line has gone out.
bool Commands_IsChatCommand(const char* text);
void Commands_QueueChat(int iSlot, const char* text);

// Runs whatever the say hook queued. Call once per GameFrame.
void Commands_ProcessQueue();
