import { EngineLoop } from './loop'

const canvas = document.getElementById('canvas') as HTMLCanvasElement

let x = 80
let y = 80
let vx = 220
let vy = 160
const SIZE = 60

let wasmStatus = 'loading…'

async function loadCore(): Promise<void> {
  try {
    const response = await fetch('/wasm/core.wasm')
    const { instance } = await WebAssembly.instantiateStreaming(response)
    const ping = instance.exports.engine_ping as () => number
    wasmStatus = ping() === 1 ? 'WASM: OK' : 'WASM: unexpected value'
  } catch {
    wasmStatus = 'WASM: failed'
  }
}

const loop = new EngineLoop(canvas, {
  update(dt) {
    x += vx * dt
    y += vy * dt
    const w = window.innerWidth
    const h = window.innerHeight
    if (x < 0)        { x = 0;        vx =  Math.abs(vx) }
    if (y < 0)        { y = 0;        vy =  Math.abs(vy) }
    if (x + SIZE > w) { x = w - SIZE; vx = -Math.abs(vx) }
    if (y + SIZE > h) { y = h - SIZE; vy = -Math.abs(vy) }
  },
  render(ctx) {
    ctx.clearRect(0, 0, window.innerWidth, window.innerHeight)
    ctx.fillStyle = '#7c3aed'
    ctx.fillRect(x, y, SIZE, SIZE)
    ctx.fillStyle = '#ffffff'
    ctx.font = '14px monospace'
    ctx.fillText(wasmStatus, 12, 24)
  },
})

loadCore().then(() => loop.start())
