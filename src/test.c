// #include "chat.h"
// #include "tool_func.h"
// #include "utils.h"
// #include <rtthread.h>

// #include "cJSON.h"
// #include <string.h>

// const char rule_text[] = "1. When the user's requirement is unclear, infer the real demand and confirm with the user\n2. Before calling tools, state your intention first; do not predict results before receiving tool output\n3. If tool calling fails, analyze failure causes before trying alternative methods\n4. File operations: Check file existence and read original content before writing or editing\n5. After writing/editing files, re-read content to verify correctness\n6.use english to answer";


// static cJSON* create_tool_item(
//     const char* func_name,
//     const char* desc,
//     cJSON* params_obj
// )
// {
//     // 外层工具对象 { "type":"function", "function": {...} }
//     cJSON* tool_root = cJSON_CreateObject();
//     cJSON_AddStringToObject(tool_root, "type", "function");

//     // function 子对象
//     cJSON* func_obj = cJSON_CreateObject();
//     cJSON_AddStringToObject(func_obj, "name", func_name);
//     cJSON_AddStringToObject(func_obj, "description", desc);
//     cJSON_AddItemToObject(func_obj, "parameters", params_obj);

//     cJSON_AddItemToObject(tool_root, "function", func_obj);
//     return tool_root;
// }

// static cJSON* create_param_obj(
//     cJSON* props_arr,
//     cJSON* required_arr
// )
// {
//     cJSON* param_root = cJSON_CreateObject();
//     cJSON_AddStringToObject(param_root, "type", "object");
//     cJSON_AddItemToObject(param_root, "properties", props_arr);
//     cJSON_AddItemToObject(param_root, "required", required_arr);
//     return param_root;
// }

// static cJSON* create_property(const char* name, const char* type, const char* desc)
// {
//     cJSON* prop = cJSON_CreateObject();
//     cJSON_AddStringToObject(prop, "type", type);
//     cJSON_AddStringToObject(prop, "description", desc);
//     return prop;
// }

// /**
//  * @brief 构建完整工具列表，对应Python 4个函数 add/mul/compare/count_letter_in_string
//  * @return cJSON数组，调用方使用完成后必须 cJSON_Delete()
//  */
// cJSON* build_tools_json(void)
// {
//     cJSON* tools_root = cJSON_CreateArray();
//     if (!tools_root)
//     {
//         return RT_NULL;
//     }

//     // ====================== 工具1: add ======================
//     {
//         cJSON* props = cJSON_CreateObject();
//         cJSON_AddItemToObject(props, "a", create_property("a", "int", "A number"));
//         cJSON_AddItemToObject(props, "b", create_property("b", "int", "A number"));

//         cJSON* required = cJSON_CreateArray();
//         cJSON_AddItemToArray(required, cJSON_CreateString("a"));
//         cJSON_AddItemToArray(required, cJSON_CreateString("b"));

//         cJSON* params = create_param_obj(props, required);
//         cJSON* tool = create_tool_item(
//             "add",
//             "Compute the sum of two numbers",
//             params
//         );
//         cJSON_AddItemToArray(tools_root, tool);
//     }

//     // ====================== 工具2: mul ======================
//     {
//         cJSON* props = cJSON_CreateObject();
//         cJSON_AddItemToObject(props, "a", create_property("a", "int", "A number"));
//         cJSON_AddItemToObject(props, "b", create_property("b", "int", "A number"));

//         cJSON* required = cJSON_CreateArray();
//         cJSON_AddItemToArray(required, cJSON_CreateString("a"));
//         cJSON_AddItemToArray(required, cJSON_CreateString("b"));

//         cJSON* params = create_param_obj(props, required);
//         cJSON* tool = create_tool_item(
//             "mul",
//             "Calculate the product of two numbers",
//             params
//         );
//         cJSON_AddItemToArray(tools_root, tool);
//     }

//     // ====================== 工具3: compare ======================
//     {
//         cJSON* props = cJSON_CreateObject();
//         cJSON_AddItemToObject(props, "a", create_property("a", "float", "A number"));
//         cJSON_AddItemToObject(props, "b", create_property("b", "float", "A number"));

//         cJSON* required = cJSON_CreateArray();
//         cJSON_AddItemToArray(required, cJSON_CreateString("a"));
//         cJSON_AddItemToArray(required, cJSON_CreateString("b"));

//         cJSON* params = create_param_obj(props, required);
//         cJSON* tool = create_tool_item(
//             "compare",
//             "Compare two number, which one is bigger",
//             params
//         );
//         cJSON_AddItemToArray(tools_root, tool);
//     }
//     // char* str = cJSON_PrintUnformatted(tools_root);
//     // int len = rt_strlen(str);
//     // for (int i = 0; i < len; i++)
//     // {
//     //     /* code */
//     //     rt_kprintf("%c",str[i]);
//     // }
    

//     return tools_root;
// }


// static void chat_response_free(ChatResponse_t resp)
// {
//     if (!resp) return;
//     rt_free(resp->reasoning);
//     rt_free(resp->context);
//     cJSON_Delete(resp->tool_call);
//     rt_free(resp);
// }

// static void print_messages(cJSON* msg_array)
// {
//     if (!msg_array) return;
//     char* dump = cJSON_PrintUnformatted(msg_array);
//     int dump_len = rt_strlen(dump);
//     rt_kprintf("\n=== Full Messages Context ================================\n");
//     for (int i = 0; i < dump_len; i++)
//     {
//         /* code */
//         rt_kprintf("%c",dump[i]);
//     }
    
    
//     cJSON_free(dump); // cJSON内存必须用cJSON_free释放，禁止rt_free
// }

// // Single conversation test entry, supports multi-turn tool loops until no tool_call is returned
// static void run_test_case(const char* user_prompt)
// {
//     rt_kprintf("\n===== Testing problem: %s =====\n", user_prompt);

//     // 1. Initialize conversation context messages
//     cJSON* messages = cJSON_CreateArray();
//     cJSON* system_msg = cJSON_CreateObject();
//     cJSON* user_msg = cJSON_CreateObject();

//     cJSON_AddStringToObject(system_msg, "role", "system");
//     cJSON_AddStringToObject(system_msg, "content", rule_text);
//     // Deep copy into array, avoid double free
//     cJSON_AddItemToArray(messages, cJSON_Duplicate(system_msg, cJSON_True));
//     cJSON_Delete(system_msg);

//     cJSON_AddStringToObject(user_msg, "role", "user");
//     cJSON_AddStringToObject(user_msg, "content", user_prompt);
//     cJSON_AddItemToArray(messages, user_msg);

//     // 2. Construct tool list
//     cJSON* tools = build_tools_json();

//     // Multi-turn tool call loop, max 10 rounds to prevent infinite loop
//     const int loop_max = 10;
//     int loop_cnt = 0;
//     ChatResponse_t resp = RT_NULL;

//     while (loop_cnt < loop_max)
//     {
//         loop_cnt++;
//         rt_kprintf("\n---------- Round %d conversation request ----------\n", loop_cnt);
//         // Send streaming chat request
//         resp = chat(messages, tools, 1024, print_reasoning, print_tool_call, print_context);
//         if (resp == RT_NULL)
//         {
//             rt_kprintf("Chat request failed, terminating this case\n");
//             goto clean_base;
//         }

//         // No tool calls received: final answer obtained, exit loop
//         if (cJSON_GetArraySize(resp->tool_call) == 0)
//         {
//             const char* ans = resp->context ? resp->context : "";
//             rt_kprintf("\nModel final answer: %s\n", ans);
//             // Store assistant text message
//             cJSON* assist_final = cJSON_CreateObject();
//             cJSON_AddStringToObject(assist_final, "role", "assistant");
//             cJSON_AddStringToObject(assist_final, "content", ans);
//             cJSON_AddItemToArray(messages, assist_final);
//             goto clean_resp;
//         }

//         // Save assistant message with tool_calls to context
//         cJSON* assist_msg = cJSON_CreateObject();
//         cJSON_AddStringToObject(assist_msg, "role", "assistant");
//         cJSON_AddItemToObject(assist_msg, "tool_calls", cJSON_Duplicate(resp->tool_call, cJSON_True));
//         cJSON_AddItemToArray(messages, assist_msg);

//         // Execute all tool calls
//         int tool_count = cJSON_GetArraySize(resp->tool_call);
//         rt_kprintf("\nDetected %d tool invocation(s)\n", tool_count);
//         for (int t = 0; t < tool_count; t++)
//         {
//             cJSON* tc_item = cJSON_GetArrayItem(resp->tool_call, t);
//             if (!tc_item) continue;

//             cJSON* tc_func = cJSON_GetObjectItemCaseSensitive(tc_item, "function");

//             // Validate function node
//             if (!tc_func || !cJSON_IsObject(tc_func))
//             {
//                 rt_kprintf("Tool call %d missing function field, skip\n", t);
//                 continue;
//             }

//             cJSON* name_node = cJSON_GetObjectItemCaseSensitive(tc_func, "name");
//             cJSON* args_node = cJSON_GetObjectItemCaseSensitive(tc_func, "arguments");
//             cJSON* id_node = cJSON_GetObjectItem(tc_item,"id");
//             if (!name_node || !cJSON_IsString(name_node) || !args_node || !cJSON_IsString(args_node))
//             {
//                 rt_kprintf("Tool call %d invalid name/arguments, skip\n", t);
//                 continue;
//             }

//             const char* func_name = name_node->valuestring;
//             const char* args_str = args_node->valuestring;
//             const char* id_str = id_node->valuestring;

//             // Parse tool arguments
//             cJSON* args_json = cJSON_Parse(args_str);
//             if (!args_json)
//             {
//                 rt_kprintf("Tool %s parse arguments failed, skip execution\n", func_name);
//                 continue;
//             }

//             ToolRet_t tool_ret = tool_dispatch(func_name, args_json);
//             const char* ret_content = tool_ret.result ? tool_ret.result : "";
//             rt_kprintf("[Local Tool %s Execution Result] %s\n", func_name, ret_content);

//             // Append tool response message
//             cJSON* tool_msg = cJSON_CreateObject();
//             cJSON_AddStringToObject(tool_msg, "role", "tool");
//             cJSON_AddStringToObject(tool_msg, "tool_call_id",id_str);
//             cJSON_AddStringToObject(tool_msg, "content", ret_content);
//             cJSON_AddItemToArray(messages, tool_msg);

//             cJSON_Delete(args_json);
//         }


//         // Print full messages after updating context
//         // print_messages(messages);

//         // Release current response
//         chat_response_free(resp);
//         resp = RT_NULL;
//     }

//     if (loop_cnt >= loop_max)
//     {
//         rt_kprintf("Maximum tool loop count %d reached, forced termination\n", loop_max);
//     }

// clean_resp:
//     chat_response_free(resp);
// clean_base:
//     cJSON_Delete(messages);
//     cJSON_Delete(tools);
// }

// // 测试入口函数，对应Python prompts数组
// static void test_siliconflow_function_call(void* parameter)
// {
//     // 两个测试用例，和Python完全一致
//     const char* case1 = "list tools and please compute :5+5";
//     const char* case2 = "Which is smaller, 9.11 or 9.9?";
//     run_test_case(case1);
//     rt_thread_mdelay(1000);
//     run_test_case(case2);
// }

// void entry_test(){
//     rt_thread_t test_agent = rt_thread_create("agent_test",test_siliconflow_function_call,RT_NULL,10480,10,10);

//     rt_thread_startup(test_agent);

//     return;
// }

// MSH_CMD_EXPORT(entry_test,entry_test);

