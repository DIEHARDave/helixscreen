// SPDX-License-Identifier: GPL-3.0-or-later
// The loop's three outside dependencies (GitHub, email, R2) as small interfaces, plus
// the production adapters. Loop logic depends on the interfaces only.

import { githubFetch, getInstallationToken } from '../../../shared/github'
import { sendEmail, type EmailMessage } from '../../../shared/email'
import { fingerprintInBody } from './fingerprint'

export interface IssueRef {
  number: number
  state: 'open' | 'closed'
  url: string
  body: string
  /** ISO week of the last telemetry comment. Filled from LoopState by the caller, never by the tracker. */
  lastTelemetryCommentWeek: string | null
}

export interface IssueTracker {
  /** Searches open and closed issues labelled telemetry for the fingerprint's marker. */
  findByFingerprint(fp: string): Promise<IssueRef | null>
  create(title: string, body: string, labels: string[]): Promise<IssueRef>
  comment(issue: number, body: string): Promise<void>
  reopen(issue: number): Promise<void>
}

export interface Mailer {
  send(message: EmailMessage): Promise<void>
}

export interface ObjectStore {
  get(key: string): Promise<string | null>
  put(key: string, value: string): Promise<void>
  list(prefix: string): Promise<string[]>
}

export interface LoopEnv {
  GITHUB_REPO?: string
  GITHUB_APP_ID?: string
  GITHUB_APP_PRIVATE_KEY?: string
  RESEND_API_KEY?: string
  NOTIFICATION_EMAIL?: string
  EMAIL_FROM?: string
  DASHBOARD_URL?: string
}

const USER_AGENT = 'HelixScreen-Telemetry-Loop'

/** Injection points for tests; production uses the globals and a GitHub App token. */
export interface GithubDeps {
  fetch?: typeof fetch
  getToken?: () => Promise<string>
}

interface GithubIssue {
  number: number
  state: string
  html_url: string
  body: string | null
}

function toRef(issue: GithubIssue): IssueRef {
  return {
    number: issue.number,
    state: issue.state === 'closed' ? 'closed' : 'open',
    url: issue.html_url,
    body: issue.body ?? '',
    lastTelemetryCommentWeek: null,
  }
}

export function githubIssueTracker(env: LoopEnv, deps: GithubDeps = {}): IssueTracker {
  const fetchImpl = deps.fetch ?? fetch
  let cachedToken: Promise<string> | null = null
  const token = (): Promise<string> => {
    if (deps.getToken) return deps.getToken()
    const { GITHUB_REPO: repo, GITHUB_APP_ID: appId, GITHUB_APP_PRIVATE_KEY: pem } = env
    if (!repo || !appId || !pem) return Promise.reject(new Error('GitHub App is not configured'))
    const [owner, name] = repo.split('/')
    cachedToken ??= getInstallationToken(appId, pem, owner, name)
    return cachedToken
  }
  const repo = env.GITHUB_REPO ?? ''

  async function call(path: string, options: RequestInit = {}): Promise<Response> {
    const res = await githubFetch(path, await token(), options, USER_AGENT, fetchImpl)
    if (!res.ok) throw new Error(`GitHub ${options.method ?? 'GET'} ${path} ${res.status}: ${await res.text()}`)
    return res
  }
  const jsonInit = (method: string, payload: unknown): RequestInit => ({
    method,
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(payload),
  })

  return {
    async findByFingerprint(fp) {
      const q = `repo:${repo} is:issue label:telemetry "telemetry-fp: ${fp}" in:body`
      const res = await call(`/search/issues?q=${encodeURIComponent(q)}&per_page=30`)
      const { items } = (await res.json()) as { items: GithubIssue[] }
      // Search is fuzzy; only an exact marker match is the same finding.
      const exact = items.filter((i) => fingerprintInBody(i.body ?? '') === fp)
      exact.sort((a, b) => Number(b.state === 'open') - Number(a.state === 'open') || a.number - b.number)
      return exact.length ? toRef(exact[0]) : null
    },
    async create(title, body, labels) {
      const res = await call(`/repos/${repo}/issues`, jsonInit('POST', { title, body, labels }))
      return toRef((await res.json()) as GithubIssue)
    },
    async comment(issue, body) {
      await call(`/repos/${repo}/issues/${issue}/comments`, jsonInit('POST', { body }))
    },
    async reopen(issue) {
      await call(`/repos/${repo}/issues/${issue}`, jsonInit('PATCH', { state: 'open' }))
    },
  }
}

export function resendMailer(env: LoopEnv, fetchImpl: typeof fetch = fetch): Mailer {
  return {
    async send(message) {
      const { RESEND_API_KEY: apiKey, NOTIFICATION_EMAIL: to, EMAIL_FROM: from } = env
      if (!apiKey || !to || !from) throw new Error('Resend is not configured')
      await sendEmail({ apiKey, from, to }, message, fetchImpl)
    },
  }
}

export function r2Store(bucket: R2Bucket): ObjectStore {
  return {
    async get(key) {
      const obj = await bucket.get(key)
      return obj ? await obj.text() : null
    },
    async put(key, value) {
      await bucket.put(key, value)
    },
    async list(prefix) {
      const keys: string[] = []
      let cursor: string | undefined
      do {
        const page = await bucket.list({ prefix, cursor })
        keys.push(...page.objects.map((o) => o.key))
        cursor = page.truncated ? page.cursor : undefined
      } while (cursor)
      return keys
    },
  }
}
