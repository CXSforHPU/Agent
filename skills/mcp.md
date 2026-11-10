# Using remote MCP tools

Use this skill when the user asks to reach an external system through the MCP (Model Context Protocol) server that the RT-Thread Agent is configured with, for example reading a file on the host PC, searching for files, running a tool that Windows exposes through the On-Device Registry, or driving the desktop of the host PC through Windows-MCP.

1. The remote tools arrive in your tool list with an `mcp_` prefix, for example `mcp_read_text_file`, `mcp_win__click` or `mcp_web_search`. Call them by the exact name from the tool list; never invent an MCP tool name, and never guess a parameter name. The parameter names and types come from the server's own schema, which is shown with the tool.

2. The client connects once at agent startup. If the server was unreachable at that moment, the remote tools are absent from the tool list entirely. Do not simulate an MCP call, and do not fall back to a local tool with a similar name. Tell the user the server was unreachable and ask them to check the endpoint and that the gateway is running.

3. Read the description before calling. The client appends `Read-only.` to tools whose server-side annotation marks them read-only. Anything without that marker may change the host, so it needs explicit user authorization first, even when the request sounds harmless.

4. Desktop tools act on the user's real computer: they move the mouse, type, open windows, run shell commands, kill processes and edit the registry. Treat each one as an action on a live machine, not a query. Get explicit authorization before the first such call in a request, and name the tool you are about to call and what it will do. Read-only desktop tools such as the display inventory, the screenshot tools, the wait tools and web scraping need no authorization.

5. Do not use `mcp_win__powershell` or `mcp_win__registry` to work around a limit the user set, to reach files outside the scope they gave you, or to change the machine's configuration in ways they did not ask for. When a desktop action needs a capability you were not given, ask instead.

6. A tool result is text. When the server reports an error, the text explains why and the call itself fails; report that reason instead of retrying blindly. When a result is empty or truncated, say so rather than claiming the operation completed. Image content never reaches you: the gateway stores the picture on the file server and replaces it with a short note that carries the download address. You cannot see the picture yourself, so hand the address to the user instead of describing what is on the screen. Audio content works the same way: the gateway stores the clip on the file server and leaves a note with the download address. You cannot hear the audio yourself, so hand the address over as well.

7. Arguments are passed through unchanged to the server, so the server validates them. Absolute paths outside the server's allowed root are rejected by the server; pass paths the user named, and ask when a path is ambiguous.

8. Each call is a separate HTTP request to the server, so a long-running operation may time out. The client waits 30 seconds by default. A timeout means the outcome is unknown; check the state before repeating an action that changes files or the desktop.

9. Report only what the result establishes: which tool was called, what it returned, and what failed. Do not claim a file was changed, a click landed, or a remote action happened unless the tool result says so.

10. The gateway ships a web search tool named `web_search`, so it reaches you as `mcp_web_search`. It is read-only and needs no authorization. Use it for facts the conversation does not carry yet. The result is a list of titles, links and snippets; quote the links you relied on instead of stating the fact on its own. When the search service is unreachable, the text says so, and that is a reason to ask the user rather than to guess the answer.
