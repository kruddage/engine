import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest'
import { EngineLoop } from './loop'

function makeMockCanvas() {
  const ctx = {
    setTransform: vi.fn(),
    clearRect: vi.fn(),
    fillRect: vi.fn(),
  } as unknown as CanvasRenderingContext2D

  const canvas = {
    getContext: vi.fn(() => ctx),
    width: 0,
    height: 0,
    style: { width: '', height: '' },
  } as unknown as HTMLCanvasElement

  return { canvas, ctx }
}

describe('EngineLoop', () => {
  let rafId = 0
  const rafCallbacks = new Map<number, FrameRequestCallback>()
  let mockRaf: ReturnType<typeof vi.fn>
  let mockCaf: ReturnType<typeof vi.fn>

  let windowListeners: Record<string, () => void> = {}
  let documentListeners: Record<string, () => void> = {}
  let documentHidden = false

  // Fires the most recently registered RAF callback
  function fireRaf(timestamp: number) {
    rafCallbacks.get(rafId)?.(timestamp)
  }

  beforeEach(() => {
    rafId = 0
    rafCallbacks.clear()
    windowListeners = {}
    documentListeners = {}
    documentHidden = false

    mockRaf = vi.fn((cb: FrameRequestCallback) => {
      rafCallbacks.set(++rafId, cb)
      return rafId
    })
    mockCaf = vi.fn((id: number) => { rafCallbacks.delete(id) })

    vi.stubGlobal('requestAnimationFrame', mockRaf)
    vi.stubGlobal('cancelAnimationFrame', mockCaf)
    vi.stubGlobal('window', {
      devicePixelRatio: 2,
      innerWidth: 800,
      innerHeight: 600,
      addEventListener: vi.fn((type: string, cb: () => void) => { windowListeners[type] = cb }),
      removeEventListener: vi.fn((type: string) => { delete windowListeners[type] }),
    })
    vi.stubGlobal('document', {
      get hidden() { return documentHidden },
      addEventListener: vi.fn((type: string, cb: () => void) => { documentListeners[type] = cb }),
      removeEventListener: vi.fn((type: string) => { delete documentListeners[type] }),
    })
  })

  afterEach(() => {
    vi.unstubAllGlobals()
  })

  it('throws when canvas has no 2D context', () => {
    const canvas = { getContext: vi.fn(() => null), style: {} } as unknown as HTMLCanvasElement
    expect(() => new EngineLoop(canvas, { update: vi.fn(), render: vi.fn() })).toThrow('Failed to get 2D rendering context')
  })

  it('starts in stopped state', () => {
    const { canvas } = makeMockCanvas()
    const loop = new EngineLoop(canvas, { update: vi.fn(), render: vi.fn() })
    expect(loop.isRunning).toBe(false)
    expect(loop.isPaused).toBe(false)
  })

  it('sizes canvas to window dimensions with DPR on construction', () => {
    const { canvas, ctx } = makeMockCanvas()
    new EngineLoop(canvas, { update: vi.fn(), render: vi.fn() })
    expect(canvas.width).toBe(1600)   // 800 * dpr(2)
    expect(canvas.height).toBe(1200)  // 600 * dpr(2)
    expect(canvas.style.width).toBe('800px')
    expect(canvas.style.height).toBe('600px')
    expect(ctx.setTransform).toHaveBeenCalledWith(2, 0, 0, 2, 0, 0)
  })

  it('start schedules RAF and sets isRunning', () => {
    const { canvas } = makeMockCanvas()
    const loop = new EngineLoop(canvas, { update: vi.fn(), render: vi.fn() })
    loop.start()
    expect(loop.isRunning).toBe(true)
    expect(mockRaf).toHaveBeenCalledOnce()
  })

  it('start is a no-op when already running', () => {
    const { canvas } = makeMockCanvas()
    const loop = new EngineLoop(canvas, { update: vi.fn(), render: vi.fn() })
    loop.start()
    loop.start()
    expect(mockRaf).toHaveBeenCalledOnce()
  })

  it('first tick has dt=0, subsequent ticks measure elapsed time', () => {
    const { canvas } = makeMockCanvas()
    const update = vi.fn()
    const loop = new EngineLoop(canvas, { update, render: vi.fn() })
    loop.start()

    fireRaf(1000)           // first tick: dt = 0
    fireRaf(1016)           // second tick: dt = 0.016

    expect(update).toHaveBeenCalledTimes(2)
    expect(update.mock.calls[0][0]).toBe(0)
    expect(update.mock.calls[1][0]).toBeCloseTo(0.016, 3)
  })

  it('caps dt at 100ms to prevent spiral of death', () => {
    const { canvas } = makeMockCanvas()
    const update = vi.fn()
    const loop = new EngineLoop(canvas, { update, render: vi.fn() })
    loop.start()

    fireRaf(0)
    fireRaf(5000)  // 5s gap — should be capped

    expect(update.mock.calls[1][0]).toBe(0.1)
  })

  it('calls render on each tick', () => {
    const { canvas, ctx } = makeMockCanvas()
    const render = vi.fn()
    const loop = new EngineLoop(canvas, { update: vi.fn(), render })
    loop.start()
    fireRaf(0)
    fireRaf(16)
    expect(render).toHaveBeenCalledTimes(2)
    expect(render.mock.calls[0][0]).toBe(ctx)
  })

  it('each tick re-schedules the next RAF', () => {
    const { canvas } = makeMockCanvas()
    const loop = new EngineLoop(canvas, { update: vi.fn(), render: vi.fn() })
    loop.start()
    fireRaf(0)
    fireRaf(16)
    expect(mockRaf).toHaveBeenCalledTimes(3)  // start + 2 ticks
  })

  it('pause cancels RAF and sets isPaused', () => {
    const { canvas } = makeMockCanvas()
    const loop = new EngineLoop(canvas, { update: vi.fn(), render: vi.fn() })
    loop.start()
    loop.pause()
    expect(loop.isPaused).toBe(true)
    expect(mockCaf).toHaveBeenCalled()
  })

  it('pause is a no-op when not running', () => {
    const { canvas } = makeMockCanvas()
    const loop = new EngineLoop(canvas, { update: vi.fn(), render: vi.fn() })
    loop.pause()
    expect(loop.isPaused).toBe(false)
    expect(mockCaf).not.toHaveBeenCalled()
  })

  it('resume restarts RAF and clears isPaused', () => {
    const { canvas } = makeMockCanvas()
    const loop = new EngineLoop(canvas, { update: vi.fn(), render: vi.fn() })
    loop.start()
    loop.pause()
    loop.resume()
    expect(loop.isPaused).toBe(false)
    expect(mockRaf).toHaveBeenCalledTimes(2)  // start + resume
  })

  it('resume resets lastTime so dt is 0 on first tick after resume', () => {
    const { canvas } = makeMockCanvas()
    const update = vi.fn()
    const loop = new EngineLoop(canvas, { update, render: vi.fn() })
    loop.start()
    fireRaf(0)
    fireRaf(500)   // advance time significantly
    loop.pause()
    loop.resume()
    fireRaf(9999)  // big timestamp but lastTime was reset → dt = 0
    expect(update.mock.calls[2][0]).toBe(0)
  })

  it('resume is a no-op when not paused', () => {
    const { canvas } = makeMockCanvas()
    const loop = new EngineLoop(canvas, { update: vi.fn(), render: vi.fn() })
    loop.start()
    loop.resume()
    expect(mockRaf).toHaveBeenCalledOnce()
  })

  it('stop terminates the loop and removes event listeners', () => {
    const { canvas } = makeMockCanvas()
    const loop = new EngineLoop(canvas, { update: vi.fn(), render: vi.fn() })
    loop.start()
    loop.stop()
    expect(loop.isRunning).toBe(false)
    expect(mockCaf).toHaveBeenCalled()
    expect(windowListeners['resize']).toBeUndefined()
    expect(documentListeners['visibilitychange']).toBeUndefined()
  })

  it('stop is a no-op when already stopped', () => {
    const { canvas } = makeMockCanvas()
    const loop = new EngineLoop(canvas, { update: vi.fn(), render: vi.fn() })
    loop.stop()
    expect(mockCaf).not.toHaveBeenCalled()
  })

  it('suspends loop when tab becomes hidden', () => {
    const { canvas } = makeMockCanvas()
    const loop = new EngineLoop(canvas, { update: vi.fn(), render: vi.fn() })
    loop.start()

    documentHidden = true
    documentListeners['visibilitychange']?.()

    expect(mockCaf).toHaveBeenCalled()
    expect(loop.isPaused).toBe(false)  // auto-suspend is not user pause
  })

  it('resumes loop when tab becomes visible again', () => {
    const { canvas } = makeMockCanvas()
    const loop = new EngineLoop(canvas, { update: vi.fn(), render: vi.fn() })
    loop.start()

    documentHidden = true
    documentListeners['visibilitychange']?.()
    const rafCountAfterHide = (mockRaf as ReturnType<typeof vi.fn>).mock.calls.length

    documentHidden = false
    documentListeners['visibilitychange']?.()

    expect((mockRaf as ReturnType<typeof vi.fn>).mock.calls.length).toBeGreaterThan(rafCountAfterHide)
  })

  it('visibility change does not auto-resume a manually paused loop', () => {
    const { canvas } = makeMockCanvas()
    const loop = new EngineLoop(canvas, { update: vi.fn(), render: vi.fn() })
    loop.start()
    loop.pause()

    documentHidden = true
    documentListeners['visibilitychange']?.()
    documentHidden = false
    documentListeners['visibilitychange']?.()

    expect(loop.isPaused).toBe(true)  // still user-paused
  })

  it('resize event re-fits the canvas', () => {
    const { canvas, ctx } = makeMockCanvas()
    new EngineLoop(canvas, { update: vi.fn(), render: vi.fn() })
    // setTransform called once in constructor; simulate resize
    windowListeners['resize']?.()
    expect(ctx.setTransform).toHaveBeenCalledTimes(2)
  })
})
