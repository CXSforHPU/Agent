/**
 * 后端接口封装。统一处理 JWT 与 {success, message, data} 响应包。
 * 所有路径走同源 /api，由 vite 代理到 8080。
 */

const TOKEN_KEY = 'agent-console-token'

export function getToken() {
  return localStorage.getItem(TOKEN_KEY) || ''
}

export function setToken(token) {
  if (token) {
    localStorage.setItem(TOKEN_KEY, token)
  } else {
    localStorage.removeItem(TOKEN_KEY)
  }
}

export function isLoggedIn() {
  return Boolean(getToken())
}

class ApiError extends Error {
  constructor(message, status) {
    super(message)
    this.status = status
  }
}

async function request(path, { method = 'GET', body, auth = true } = {}) {
  const headers = { 'Content-Type': 'application/json' }
  if (auth) {
    const token = getToken()
    if (token) {
      headers.Authorization = `Bearer ${token}`
    }
  }

  let resp
  try {
    resp = await fetch(`/api${path}`, {
      method,
      headers,
      body: body === undefined ? undefined : JSON.stringify(body),
    })
  } catch (e) {
    // 后端没起来时 fetch 会抛 TypeError
    throw new ApiError(`连不上后端：${e.message}`, 0)
  }

  let payload = null
  try {
    payload = await resp.json()
  } catch {
    throw new ApiError(`后端返回了非 JSON 内容（HTTP ${resp.status}）`, resp.status)
  }

  if (resp.status === 401) {
    setToken('')
    throw new ApiError(payload.message || '登录已过期，请重新登录', 401)
  }
  if (!resp.ok || payload.success === false) {
    throw new ApiError(payload.message || `请求失败（HTTP ${resp.status}）`, resp.status)
  }
  return payload.data
}

/**
 * 二进制与 multipart 的请求。返回 Response，调用方自己看状态码和 content-type。
 * TTS 返回 WAV 字节，STT 要发 FormData，都不能走上面的 JSON 封装。
 */
async function rawRequest(path, { method = 'GET', body } = {}) {
  const headers = {}
  const token = getToken()
  if (token) {
    headers.Authorization = `Bearer ${token}`
  }

  let resp
  try {
    resp = await fetch(`/api${path}`, { method, headers, body })
  } catch (e) {
    throw new ApiError(`连不上后端：${e.message}`, 0)
  }

  if (resp.status === 401) {
    setToken('')
    throw new ApiError('登录已过期，请重新登录', 401)
  }
  return resp
}

export const api = {
  register: (username, password) =>
    request('/auth/register', { method: 'POST', body: { username, password }, auth: false }),
  login: (username, password) =>
    request('/auth/login', { method: 'POST', body: { username, password }, auth: false }),
  me: () => request('/auth/me'),

  listDevices: () => request('/devices'),
  createDevice: (data) => request('/devices', { method: 'POST', body: data }),
  updateDevice: (id, data) => request(`/devices/${id}`, { method: 'PUT', body: data }),
  deleteDevice: (id) => request(`/devices/${id}`, { method: 'DELETE' }),
  probeDevice: (id) => request(`/devices/${id}/probe`, { method: 'POST' }),
  pushDeviceConfig: (id, data) => request(`/devices/${id}/config`, { method: 'POST', body: data }),
  pushDeviceCard: (id, data) => request(`/devices/${id}/card`, { method: 'POST', body: data }),

  /** 这台设备最近的上报事件，新的在前。 */
  listDeviceEvents: (id) => request(`/devices/${id}/events`),

  /**
   * 设备上报事件流。端侧 agent 自己产生的结论（告警、传感器越限）从这条流过来。
   * onEvent 收到 {kind, text, createdAt}。流断开时正常返回，调用方自己决定要不要重连。
   */
  async deviceEventStream(id, onEvent, signal) {
    const headers = {}
    const token = getToken()
    if (token) {
      headers.Authorization = `Bearer ${token}`
    }

    let resp
    try {
      resp = await fetch(`/api/devices/${id}/events/stream`, { headers, signal })
    } catch (e) {
      if (e.name === 'AbortError') return
      throw new ApiError(`连不上后端：${e.message}`, 0)
    }

    if (resp.status === 401) {
      setToken('')
      throw new ApiError('登录已过期，请重新登录', 401)
    }
    if (!resp.ok) {
      throw new ApiError(`请求失败（HTTP ${resp.status}）`, resp.status)
    }

    const reader = resp.body.getReader()
    const decoder = new TextDecoder('utf-8')
    let buffer = ''
    try {
      for (;;) {
        const { done, value } = await reader.read()
        if (done) break
        buffer += decoder.decode(value, { stream: true })
        let index
        while ((index = buffer.indexOf('\n\n')) >= 0) {
          const frame = buffer.slice(0, index)
          buffer = buffer.slice(index + 2)
          for (const line of frame.split('\n')) {
            if (!line.startsWith('data:')) continue
            const payload = line.slice(5).trim()
            if (!payload) continue
            try {
              onEvent(JSON.parse(payload))
            } catch {
              // 坏帧丢掉，不影响后面的事件
            }
          }
        }
      }
    } catch (e) {
      if (e.name !== 'AbortError') throw e
    }
  },

  listConversations: (deviceId) =>
    request(`/conversations${deviceId ? `?deviceId=${deviceId}` : ''}`),
  getConversation: (id) => request(`/conversations/${id}`),
  createConversation: (data) => request('/conversations', { method: 'POST', body: data }),
  renameConversation: (id, title) => request(`/conversations/${id}`, { method: 'PATCH', body: { title } }),
  deleteConversation: (id) => request(`/conversations/${id}`, { method: 'DELETE' }),

  chat: (data) => request('/chat', { method: 'POST', body: data, auth: true }),

  /**
   * 流式聊天。onEvent 收到 {kind, text, conversationId}，
   * kind 取 status / reasoning / tool / content / final / error。
   * 端侧 agent 的思考、工具调用、正文分片随到随推，final 是完整答案。
   * 传 signal 可以在面板关闭时中止这次请求。
   */
  async chatStream(data, onEvent, signal) {
    const headers = { 'Content-Type': 'application/json' }
    const token = getToken()
    if (token) {
      headers.Authorization = `Bearer ${token}`
    }

    let resp
    try {
      resp = await fetch('/api/chat/stream', {
        method: 'POST',
        headers,
        body: JSON.stringify(data),
        signal,
      })
    } catch (e) {
      if (e.name === 'AbortError') throw e
      throw new ApiError(`连不上后端：${e.message}`, 0)
    }

    if (resp.status === 401) {
      setToken('')
      throw new ApiError('登录已过期，请重新登录', 401)
    }
    if (!resp.ok) {
      let msg = `请求失败（HTTP ${resp.status}）`
      const raw = await resp.text()
      try {
        msg = JSON.parse(raw).message || msg
      } catch {
        // 错误也可能是 SSE 帧（比如会话不存在），从 data 行里取原因
        const line = raw.split('\n').find((l) => l.startsWith('data:'))
        if (line) {
          try {
            msg = JSON.parse(line.slice(5).trim()).text || msg
          } catch {
            // 帧解析不了就用默认文案
          }
        }
      }
      throw new ApiError(msg, resp.status)
    }

    const reader = resp.body.getReader()
    const decoder = new TextDecoder('utf-8')
    let buffer = ''
    for (;;) {
      const { done, value } = await reader.read()
      if (done) break
      buffer += decoder.decode(value, { stream: true })
      let index
      while ((index = buffer.indexOf('\n\n')) >= 0) {
        const frame = buffer.slice(0, index)
        buffer = buffer.slice(index + 2)
        for (const line of frame.split('\n')) {
          if (!line.startsWith('data:')) continue
          const payload = line.slice(5).trim()
          if (!payload) continue
          try {
            onEvent(JSON.parse(payload))
          } catch {
            // 坏帧丢掉，不影响后面的事件
          }
        }
      }
    }
  },

  getSettings: () => request('/settings'),
  saveSettings: (data) => request('/settings', { method: 'PUT', body: data }),
  resetSetting: (key) => request(`/settings/${encodeURIComponent(key)}/reset`, { method: 'POST' }),

  emqxStatus: () => request('/ops/emqx'),
  emqxAction: (action) => request('/ops/emqx/' + action, { method: 'POST' }),
  opsStatus: () => request('/ops/status'),
  opsSettings: () => request('/ops/settings'),
  opsSaveSettings: (data) => request('/ops/settings', { method: 'PUT', body: data }),
  opsComponents: (refresh = false) =>
    request(`/ops/components?refresh=${refresh ? 'true' : 'false'}`),
  opsInstall: (target, components) => {
    const query = new URLSearchParams({ target })
    if (components && components.length) {
      query.set('components', components.join(','))
    }
    return request(`/ops/install?${query.toString()}`, { method: 'POST' })
  },
  opsJobs: () => request('/ops/jobs'),
  opsJob: (id) => request(`/ops/jobs/${encodeURIComponent(id)}`),
  opsStart: () => request('/ops/start', { method: 'POST' }),
  opsStop: () => request('/ops/stop', { method: 'POST' }),
  opsSpeakers: () => request('/ops/tts/speakers'),
  opsTtsProgress: () => request('/ops/tts/progress'),
  opsLog: (name, lines = 100) =>
    request(`/ops/log?name=${encodeURIComponent(name)}&lines=${lines}`),

  /** 合成语音，返回 WAV 的 Blob。 */
  async opsTts(text, speaker) {
    const query = new URLSearchParams({ text })
    if (speaker) {
      query.set('speaker', speaker)
    }
    const resp = await rawRequest(`/ops/tts?${query}`, { method: 'POST' })
    if (!resp.ok) {
      const type = resp.headers.get('content-type') || ''
      let message = `合成失败（HTTP ${resp.status}）`
      if (type.includes('application/json')) {
        try {
          message = (await resp.json()).message || message
        } catch {
          // 响应体不是 JSON，用默认文案
        }
      }
      throw new ApiError(message, resp.status)
    }
    return resp.blob()
  },

  /** 语音转文本，返回识别出的文本。 */
  async opsStt(file) {
    const form = new FormData()
    form.append('file', file)
    const resp = await rawRequest('/ops/stt', { method: 'POST', body: form })
    const payload = await resp.json().catch(() => null)
    if (!resp.ok || !payload || payload.success === false) {
      throw new ApiError((payload && payload.message) || `转写失败（HTTP ${resp.status}）`, resp.status)
    }
    return payload.data.text
  },
}

export { ApiError }
