export interface LoopCallbacks {
  update(dt: number): void
  render(ctx: CanvasRenderingContext2D): void
}

export class EngineLoop {
  readonly canvas: HTMLCanvasElement
  readonly ctx: CanvasRenderingContext2D

  private _running = false
  private _paused = false
  private _suspended = false  // auto-suspended by tab visibility
  private rafHandle = 0
  private lastTime: number | null = null
  private readonly callbacks: LoopCallbacks

  constructor(canvas: HTMLCanvasElement, callbacks: LoopCallbacks) {
    const ctx = canvas.getContext('2d')
    if (!ctx) throw new Error('Failed to get 2D rendering context')
    this.canvas = canvas
    this.ctx = ctx
    this.callbacks = callbacks
    this.fitCanvas()
    window.addEventListener('resize', this.onResize)
    document.addEventListener('visibilitychange', this.onVisibilityChange)
  }

  get isRunning(): boolean { return this._running }
  get isPaused(): boolean { return this._paused }

  start(): void {
    if (this._running) return
    this._running = true
    this._paused = false
    this._suspended = false
    this.lastTime = null
    this.rafHandle = requestAnimationFrame(this.tick)
  }

  stop(): void {
    if (!this._running) return
    this._running = false
    this._paused = false
    this._suspended = false
    cancelAnimationFrame(this.rafHandle)
    window.removeEventListener('resize', this.onResize)
    document.removeEventListener('visibilitychange', this.onVisibilityChange)
  }

  pause(): void {
    if (!this._running || this._paused) return
    this._paused = true
    this._suspended = false
    cancelAnimationFrame(this.rafHandle)
  }

  resume(): void {
    if (!this._running || !this._paused) return
    this._paused = false
    this.lastTime = null
    this.rafHandle = requestAnimationFrame(this.tick)
  }

  private tick = (timestamp: number): void => {
    if (!this._running || this._paused || this._suspended) return

    const dt = this.lastTime !== null
      ? Math.min((timestamp - this.lastTime) / 1000, 0.1)
      : 0
    this.lastTime = timestamp

    this.callbacks.update(dt)
    this.callbacks.render(this.ctx)

    this.rafHandle = requestAnimationFrame(this.tick)
  }

  private fitCanvas = (): void => {
    const dpr = window.devicePixelRatio || 1
    const w = window.innerWidth
    const h = window.innerHeight
    this.canvas.width = Math.round(w * dpr)
    this.canvas.height = Math.round(h * dpr)
    this.canvas.style.width = `${w}px`
    this.canvas.style.height = `${h}px`
    this.ctx.setTransform(dpr, 0, 0, dpr, 0, 0)
  }

  private onResize = (): void => {
    this.fitCanvas()
  }

  private onVisibilityChange = (): void => {
    if (!this._running || this._paused) return
    if (document.hidden) {
      this._suspended = true
      cancelAnimationFrame(this.rafHandle)
    } else if (this._suspended) {
      this._suspended = false
      this.lastTime = null
      this.rafHandle = requestAnimationFrame(this.tick)
    }
  }
}
