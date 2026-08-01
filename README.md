# Agent Framework

An intelligent agent framework built on RT-Thread that enables interaction with Large Language Models (LLMs) with tool capabilities.

## Overview

This framework allows the creation of intelligent agents that can interact with LLMs through API calls, process tool requests, and maintain conversation context. It supports tool execution, conversation state management, and message passing between components.

### demo
![demo](./pictures/demo.png)

## Architecture

The framework consists of several core components that work together to enable agent functionality:

### Core Components

- **AgentLoop**: Main agent execution loop that orchestrates the interaction with LLM and tools
- **Chat**: Handles communication with the LLM API and manages streaming responses
- **MessageHub**: Manages message passing between different components using mailboxes
- **Context**: Maintains the conversation history and context for LLM interactions
- **Tool System**: Provides tool registration, execution, and management capabilities

### UML Class Diagram

```mermaid
classDiagram
    %% ==============================================
    %% 第1层：顶层调度 AgentLoop（唯一入口）
    %% ==============================================
    class AgentLoop {
        +InitAgent()
        +AgentLoop(Context_t, cJSON*, on_reasoning, on_tool_call, on_context)
        +MainLoop_entry()
    }

    %% ==============================================
    %% 第2层：Loop直属子模块（仅与AgentLoop交互）
    %% ==============================================
    class Context {
        -payload
        +AgentContextCreate()
        +build_system_prompt(context)
        +append_user_message(context, message)
        +append_assistant_message(context, resp)
        +append_tool_message(context, tool_call_id, content)
    }
    class MessageHub {
        -input_mailbox
        -output_mailbox
        +MessageHub_create()
        +MessageHub_destroy()
        +MessageHub_put(hub, message, mb)
        +MessageHub_get(hub, mb)
    }
    class Chat {
        +chat(messages, tools, max_tokens, on_reasoning, on_tool_call, on_context)
        +chat_response_free(resp)
    }
    class ToolBase {
        +AgentToolListCreate()
        +AppendTool(tool_obj, execute_func)
        +SearchAgentToolNode(tool_obj)
        +BuildToolsJson()
        +FreeAgentToolNode(AgentToolNode_t node)
    }

    %% ==============================================
    %% 第3层：各模块内部从属对象（仅归属上层父类）
    %% ==============================================
    class Message {
        -uint8_t channel_type
        char* content
        +message_create(MessageChannelType channel_type,char *content,rt_err_t is_free_content)
        +message_destroy(Message_t message)
    }
    class ChatResponse {
        +char* reasoning
        +cJSON* context
        +cJSON* tool_call
    }
    class ToolNode {
        +void* tool_obj
        +AgentToolRet* ret
        +tool_execute_func execute_func
        +ToolNode* next
    }
    class AgentToolRet {
        +int ret_code
        +char* content
    }

    %% ==============================================
    %% 第4层：业务工具实现（仅绑定ToolNode）
    %% ==============================================
    class ToolAdd {
        +tool_add(args_obj, node)
    }
    class ToolMul {
        +tool_mul(args_obj, node)
    }
    class ToolCompare {
        +tool_compare(args_obj, node)
    }

    %% ==============================================
    %% 关联关系【严格相邻层级，无跨层连线！】
    %% ==============================================
    %% 顶层调度 聚合 第二层四大组件
    AgentLoop *-- Context
    AgentLoop *-- MessageHub
    AgentLoop *-- Chat
    AgentLoop *-- ToolBase

    %% 第二层 ↔ 第三层（仅直属内部对象）
    MessageHub *-- Message
    Chat --> ChatResponse
    ToolBase *-- ToolNode
    ToolNode *-- AgentToolRet

    %% 第四层业务工具只和同上层ToolNode关联
    ToolAdd --> ToolNode
    ToolMul --> ToolNode
    ToolCompare --> ToolNode

    %% ========= 删除所有跨层连线！=========
    %% 原跨层依赖（已移除）：
    %% Context --> MessageHub 【跨层交互由AgentLoop做调度转发，不直接依赖】
    %% ToolBase --> Chat      【工具JSON由AgentLoop中转传递给Chat，消除跨层】
```

## Key Features

### 1. Agent Loop System
- Main execution loop that orchestrates LLM interaction with tools
- Handles tool execution loops with proper context management
- Manages conversation state throughout agent interactions

### 2. Multi-Channel Messaging
- MessageHub handles communication between components
- Supports different message channels (CLI, WebNET, Assistant)
- Uses RT-Thread mailboxes for efficient messaging

### 3. Context Management
- Maintains conversation history with proper role assignment
- Supports system, user, assistant, and tool roles
- Manages context for LLM API calls

### 4. Tool System
- Dynamic tool registration and discovery
- Tool execution with parameter parsing
- Support for multiple tool types (add, multiply, compare)

### 5. Streaming API Support
- Handles streaming responses from LLMs
- Processes reasoning and content separately
- Manages tool call responses through streaming

## Component Details

### AgentLoop.c
Main agent loop that:
- Initializes components (MessageHub, Context, Tools)
- Orchestrates the chat interaction with LLM
- Processes and executes tool calls
- Manages the conversation loop

### Chat.c
Handles communication with LLM API:
- Generates requests with messages and tools
- Processes streaming responses
- Handles reasoning and content extraction
- Manages tool call parsing

### MessageHub.c
Manages message passing between components:
- Creates input/output mailboxes
- Provides messaging interface for components
- Handles message creation and destruction

### Context.c
Maintains conversation context:
- Manages conversation history
- Builds and appends different message types
- Supports different role-based messages

### Tool System
- `tool_base.c`: Base tool management functionality
- `tool_func.c`: Tool registration and initialization
- `tool_add.c`, `tool_mul.c`, `tool_compare.c`: Concrete tool implementations

## Usage

1. Initialize the agent system with `MainLoop_entry()`
2. Send messages through the MessageHub to start agent interactions
3. The agent will process messages through the LLM and execute tools as needed
4. Results are posted back through the MessageHub

## Configuration

The framework can be configured through RT-Thread package settings:
- API key, URL, and model name
- Buffer sizes for responses and streaming
- Maximum tool execution loop count

## Dependencies

- RT-Thread RTOS
- cJSON library for JSON processing
- webclient for HTTP communication
- ulog for logging

## License

This project is licensed under the MIT License.