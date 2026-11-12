#ifndef AGENT_OUTPUT_H
#define AGENT_OUTPUT_H

#include "AgentFsm.h"

void agent_notify(const char *text, void (*on_context)(const char *text));
void agent_emit_answer(Context_t context, ChatResponse_t resp,
                       void (*on_context)(const char *text));

#endif
