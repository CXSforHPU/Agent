"""联网搜索工具。

搜索服务是一个 HTTP 接口。网关按配置去问它，把结果整理成 JSON 交给模型。
默认对接 SearXNG 的 JSON 接口，不用密钥；换服务改 --web-search-url 和
--web-search-mode。

配置由网关的 config 层解析好，注册工具的时候通过 configure(context) 交过来：

    web_search.url             搜索服务地址
    web_search.key             密钥，Bing 和 Brave 用得上
    web_search.mode            对接方式，auto、searxng、bing、brave、post
    web_search.timeout         超时秒数
    web_search.max_results     一次最多返回几条
    web_search.enabled         为假就不注册这个工具

对应的环境变量是 MCP_WEB_SEARCH_URL、MCP_WEB_SEARCH_KEY、MCP_WEB_SEARCH_MODE、
MCP_WEB_SEARCH_TIMEOUT、MCP_WEB_SEARCH_MAX_RESULTS、MCP_WEB_SEARCH_ENABLED，
由 config 层统一读取。

各家搜索服务的回包形状不一样，这里按第一个带链接的列表认，所以 Bing、
Tavily、SearXNG、Brave 的回包都能解。

只用标准库。
"""
import asyncio
import json
import re
import urllib.parse
import urllib.request

from tools.base import Tool

NAME = "web_search"
DESCRIPTION = ("Search the web and return titles, links and snippets. "
               "Use it for facts that are not in the conversation yet. Read-only.")
PARAMETERS = {
    "type": "object",
    "properties": {
        "query": {"type": "string", "title": "what to search for"},
        "count": {"type": "integer", "title": "how many results to return"},
    },
    "required": ["query"],
}

# 搜索服务的默认地址。SearXNG 的公开实例不用密钥，但各实例的可用性和地区
# 不一样，自己起一个更稳：--web-search-url http://<主机>:8080/search
DEFAULT_URL = "https://searx.be"
DEFAULT_MODE = "auto"
DEFAULT_TIMEOUT = 15.0
DEFAULT_MAX_RESULTS = 5

USER_AGENT = "rt-thread-agent-mcp/1.0"

# 摘要里的标签要去掉，模型读标签没意义
TAG = re.compile(r"<[^>]+>")
SPACE = re.compile(r"\s+")

# 结果里可能用的字段名，各家搜索服务叫法不同
TITLE_KEYS = ("title", "name", "heading", "label")
URL_KEYS = ("url", "link", "href", "source_url", "targeturl")
SNIPPET_KEYS = ("snippet", "description", "content", "text", "summary",
                "abstract", "body")

# 摘要最多留这么多字符，超了模型也用不上
SNIPPET_LIMIT = 400


class WebSearchTool(Tool):
    """联网搜索。

    类里写了 namespace，对外就叫 web_search，不加 user__ 前缀。
    """

    namespace = ""
    prefix = "web__"

    def __init__(self, url=None, key=None, mode=None, timeout=None,
                 max_results=None, fetch=None):
        super().__init__()
        self.url = url
        self.key = key
        self.mode = mode
        self.timeout = timeout
        self.max_results = max_results
        self.fetch = fetch

    def configure(self, context):
        """注册时把配置读进来。

        构造时给的参数优先，其次是网关的配置，最后是默认值。
        """
        super().configure(context)
        self.url = _strip(self.url if self.url is not None
                          else context.setting("web_search.url", DEFAULT_URL))
        self.key = _strip(self.key if self.key is not None
                          else context.setting("web_search.key", ""))
        self.mode = _strip(self.mode if self.mode is not None
                           else context.setting("web_search.mode", DEFAULT_MODE)).lower()
        self.timeout = _to_float(
            self.timeout if self.timeout is not None
            else context.setting("web_search.timeout", DEFAULT_TIMEOUT),
            DEFAULT_TIMEOUT)
        self.max_results = _to_int(
            self.max_results if self.max_results is not None
            else context.setting("web_search.max_results", DEFAULT_MAX_RESULTS),
            DEFAULT_MAX_RESULTS)
        self.enabled = bool(context.setting("web_search.enabled", True))

    @property
    def name(self):
        return NAME

    @property
    def description(self):
        return DESCRIPTION

    @property
    def parameters(self):
        return PARAMETERS

    async def execute(self, **kwargs):
        """问搜索服务要一次结果，整理成 JSON 交回模型。"""
        query = str(kwargs.get("query") or "").strip()
        if not query:
            return _text("web_search 需要一个 query 参数", True)
        if not (self.url or "").strip():
            return _text("web_search 没有配置搜索服务地址，用 --web-search-url 指定",
                         True)
        count = _count(kwargs.get("count"), self.max_results or DEFAULT_MAX_RESULTS)
        try:
            payload = await asyncio.to_thread(self._take, query, count)
        except Exception as exc:
            return _text("web_search 请求失败：{}".format(exc), True)
        results = extract_results(payload, count)
        body = {"query": query, "count": count, "results": results}
        return _text(json.dumps(body, ensure_ascii=False, indent=2), False)

    def _take(self, query, count):
        """发请求拿原始回包。测试可以传 fetch 换掉。"""
        if self.fetch:
            return self.fetch(query, count)
        return fetch_results((self.url or "").strip(), self.key or "",
                             (self.mode or DEFAULT_MODE).lower(),
                             DEFAULT_TIMEOUT if self.timeout is None else self.timeout,
                             query, count)


def _strip(value):
    """去掉两头的空白。"""
    return str(value or "").strip()


def _to_float(value, fallback):
    try:
        return float(value)
    except (TypeError, ValueError):
        return fallback


def _to_int(value, fallback):
    try:
        return int(value)
    except (TypeError, ValueError):
        return fallback


def detect_mode(url):
    """从地址猜对接方式，猜不出来就按通用 JSON 接口处理。

    auto 模式下用这个结果决定请求怎么发。
    """
    lowered = (url or "").lower()
    if "searx" in lowered:
        return "searxng"
    if "bing" in lowered:
        return "bing"
    if "brave" in lowered:
        return "brave"
    return "post"


def fetch_results(url, key, mode, timeout, query, count):
    """问搜索服务要一次结果，返回原始回包。"""
    if mode == "auto":
        mode = detect_mode(url)
    if mode == "post":
        return _post(url, key, timeout, query, count)
    return _get(url, key, mode, timeout, query)


def _count(value, max_results):
    """结果条数，按配置的上限收口。"""
    try:
        count = int(value or max_results)
    except (TypeError, ValueError):
        count = max_results
    return max(1, min(count, 10))


def _first(item, keys):
    """按候选字段名找第一个非空的字符串值，字段名不区分大小写。"""
    lowered = {}
    for key, value in item.items():
        lowered.setdefault(str(key).lower(), value)
    for key in keys:
        value = lowered.get(key.lower())
        if isinstance(value, str) and value.strip():
            return value.strip()
        if isinstance(value, list) and value:
            return str(value[0])
    return ""


def _clean(text, limit=SNIPPET_LIMIT):
    """去掉标签，压掉多余空白，超长截断。"""
    if not text:
        return ""
    plain = SPACE.sub(" ", TAG.sub(" ", text)).strip()
    if len(plain) > limit:
        plain = plain[:limit].rstrip() + "..."
    return plain


def _walk_lists(node):
    """按深度优先的顺序遍历回包里的所有列表。"""
    if isinstance(node, list):
        yield node
        for item in node:
            for found in _walk_lists(item):
                yield found
    elif isinstance(node, dict):
        for value in node.values():
            for found in _walk_lists(value):
                yield found


def extract_results(payload, limit):
    """从回包里找出结果列表，最多留 limit 条。

    搜索服务没有统一的回包格式：Bing 放在 webPages.value 里，Tavily 和
    SearXNG 放在 results 里，Google 的自定义搜索放在 items 里。按第一个带
    链接的列表认，三种都能解。
    """
    for items in _walk_lists(payload):
        picked = []
        for item in items:
            if not isinstance(item, dict):
                continue
            url = _first(item, URL_KEYS)
            if not url:
                continue
            picked.append({
                "title": _clean(_first(item, TITLE_KEYS), 200) or url,
                "url": url,
                "snippet": _clean(_first(item, SNIPPET_KEYS)),
            })
        if picked:
            return picked[:limit]
    return []


def _get(url, key, mode, timeout, query):
    """GET 的对接方式：SearXNG、Bing、Brave 都是这一类。"""
    params = {"q": query}
    if mode == "searxng":
        params["format"] = "json"
    target = url
    target += "&" if "?" in target else "?"
    target += urllib.parse.urlencode(params)
    headers = {"Accept": "application/json", "User-Agent": USER_AGENT}
    if key:
        if mode == "bing":
            headers["Ocp-Apim-Subscription-Key"] = key
        elif mode == "brave":
            headers["X-Subscription-Token"] = key
        else:
            headers["Authorization"] = "Bearer {}".format(key)
    request = urllib.request.Request(target, headers=headers)
    with urllib.request.urlopen(request, timeout=timeout) as response:
        raw = response.read()
    return _decode(raw)


def _post(url, key, timeout, query, count):
    """POST 的对接方式：请求体是 JSON，密钥按两种方式都带上。

    Tavily 认请求体里的 api_key，多数服务认 Authorization 头，
    两个都发，哪边认都行。
    """
    body = {"query": query, "count": count}
    headers = {"Accept": "application/json", "User-Agent": USER_AGENT}
    if key:
        body["api_key"] = key
        headers["Authorization"] = "Bearer {}".format(key)
    headers["Content-Type"] = "application/json"
    request = urllib.request.Request(url, data=json.dumps(body).encode("utf-8"),
                                     headers=headers, method="POST")
    with urllib.request.urlopen(request, timeout=timeout) as response:
        raw = response.read()
    return _decode(raw)


def _decode(raw):
    try:
        return json.loads(raw.decode("utf-8", "replace"))
    except ValueError as exc:
        raise ValueError("搜索服务回的不是 JSON：{}".format(exc))


def _text(text, is_error):
    return {"content": [{"type": "text", "text": text}], "isError": bool(is_error)}
