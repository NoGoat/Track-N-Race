export type TrackPoint = [number, number]
export type TrackLayout = { scale: number; ox: number; oy: number }

type Rgb = [number, number, number]
const colorCache = new Map<string, Rgb>()

interface CachedPolyline {
  buffer: WebGLBuffer
  vao: WebGLVertexArrayObject
  vertices: number
}

const LINE_VERTEX = `#version 300 es
precision highp float;
layout(location = 0) in vec2 aPoint;
layout(location = 1) in vec2 aNormal;
layout(location = 2) in float aDistance;
layout(location = 3) in float aSide;

uniform vec2 uViewport;
uniform float uScale;
uniform vec2 uOffset;
uniform float uHalfWidth;
uniform float uNormalOffset;

out float vDistance;
out float vAcross;

void main() {
  // Expand past the nominal edge so the fragment shader has room for a
  // driver-independent one-pixel coverage ramp. Context MSAA is only a bonus.
  float expandedHalfWidth = uHalfWidth + 1.25;
  vec2 center = aPoint * uScale + uOffset;
  center += normalize(aNormal) * uNormalOffset;
  center += aNormal * aSide * expandedHalfWidth;
  vec2 clip = vec2(center.x / uViewport.x * 2.0 - 1.0,
                   1.0 - center.y / uViewport.y * 2.0);
  gl_Position = vec4(clip, 0.0, 1.0);
  vDistance = aDistance * uScale;
  vAcross = aSide * expandedHalfWidth;
}`

const LINE_FRAGMENT = `#version 300 es
precision highp float;
uniform vec4 uColor;
uniform float uHalfWidth;
uniform float uDashOn;
uniform float uDashPeriod;
in float vDistance;
in float vAcross;
out vec4 outColor;

void main() {
  float across = abs(vAcross);
  float edgeAA = max(fwidth(across) * 0.5, 0.15);
  float coverage = 1.0 - smoothstep(uHalfWidth - edgeAA, uHalfWidth + edgeAA, across);

  if (uDashPeriod > 0.0) {
    float phase = mod(vDistance, uDashPeriod);
    float dashAA = max(fwidth(vDistance) * 0.5, 0.15);
    float dashStart = smoothstep(0.0, dashAA, phase);
    float dashEnd = 1.0 - smoothstep(uDashOn - dashAA, uDashOn + dashAA, phase);
    coverage *= dashStart * dashEnd;
  }

  if (coverage <= 0.0) discard;
  outColor = vec4(uColor.rgb, uColor.a * coverage);
}`

const CIRCLE_VERTEX = `#version 300 es
precision highp float;
layout(location = 0) in vec2 aCenter;
layout(location = 1) in vec4 aColor;
layout(location = 2) in float aRadius;
layout(location = 3) in float aInnerRatio;

uniform vec2 uViewport;
out vec2 vLocal;
flat out vec4 vColor;
flat out float vInnerRatio;

void main() {
  vec2 corner;
  if (gl_VertexID == 0) corner = vec2(-1.0, -1.0);
  else if (gl_VertexID == 1) corner = vec2( 1.0, -1.0);
  else if (gl_VertexID == 2) corner = vec2(-1.0,  1.0);
  else corner = vec2(1.0, 1.0);
  vec2 screen = aCenter + corner * aRadius;
  vec2 clip = vec2(screen.x / uViewport.x * 2.0 - 1.0,
                   1.0 - screen.y / uViewport.y * 2.0);
  gl_Position = vec4(clip, 0.0, 1.0);
  vLocal = corner;
  vColor = aColor;
  vInnerRatio = aInnerRatio;
}`

const CIRCLE_FRAGMENT = `#version 300 es
precision highp float;
in vec2 vLocal;
flat in vec4 vColor;
flat in float vInnerRatio;
out vec4 outColor;

void main() {
  float distanceFromCenter = length(vLocal);
  float edge = max(fwidth(distanceFromCenter), 0.002);
  float outer = 1.0 - smoothstep(1.0 - edge, 1.0, distanceFromCenter);
  float inner = vInnerRatio <= 0.0
    ? 1.0
    : smoothstep(vInnerRatio - edge, vInnerRatio + edge, distanceFromCenter);
  float alpha = outer * inner;
  if (alpha <= 0.0) discard;
  outColor = vec4(vColor.rgb, vColor.a * alpha);
}`

function shader(gl: WebGL2RenderingContext, type: number, source: string): WebGLShader {
  const value = gl.createShader(type)
  if (!value) throw new Error('Unable to create track-map WebGL shader')
  gl.shaderSource(value, source)
  gl.compileShader(value)
  if (!gl.getShaderParameter(value, gl.COMPILE_STATUS)) {
    const message = gl.getShaderInfoLog(value) ?? 'Unknown shader compilation error'
    gl.deleteShader(value)
    throw new Error(message)
  }
  return value
}

function program(gl: WebGL2RenderingContext, vertex: string, fragment: string): WebGLProgram {
  const value = gl.createProgram()
  if (!value) throw new Error('Unable to create track-map WebGL program')
  const vertexShader = shader(gl, gl.VERTEX_SHADER, vertex)
  const fragmentShader = shader(gl, gl.FRAGMENT_SHADER, fragment)
  gl.attachShader(value, vertexShader)
  gl.attachShader(value, fragmentShader)
  gl.linkProgram(value)
  gl.deleteShader(vertexShader)
  gl.deleteShader(fragmentShader)
  if (!gl.getProgramParameter(value, gl.LINK_STATUS)) {
    const message = gl.getProgramInfoLog(value) ?? 'Unknown WebGL link error'
    gl.deleteProgram(value)
    throw new Error(message)
  }
  return value
}

function requiredUniform(gl: WebGL2RenderingContext, value: WebGLProgram, name: string): WebGLUniformLocation {
  const location = gl.getUniformLocation(value, name)
  if (!location) throw new Error(`Missing track-map WebGL uniform: ${name}`)
  return location
}

function parseColor(value: string): Rgb {
  const cached = colorCache.get(value)
  if (cached) return cached
  const hex = value.startsWith('#') ? value.slice(1) : value
  if (hex.length !== 6) return [1, 1, 1]
  const parsed: Rgb = [
    parseInt(hex.slice(0, 2), 16) / 255,
    parseInt(hex.slice(2, 4), 16) / 255,
    parseInt(hex.slice(4, 6), 16) / 255,
  ]
  colorCache.set(value, parsed)
  return parsed
}

/** Retained WebGL2 renderer for the Electron track map. Static polyline buffers
 * are cached by their prepared-map array identity; only the tiny car/marker
 * instance buffer is uploaded each display frame. */
export class TrackMapWebGLRenderer {
  private readonly gl: WebGL2RenderingContext
  private readonly lineProgram: WebGLProgram
  private readonly circleProgram: WebGLProgram
  private readonly lineViewport: WebGLUniformLocation
  private readonly lineScale: WebGLUniformLocation
  private readonly lineOffset: WebGLUniformLocation
  private readonly lineHalfWidth: WebGLUniformLocation
  private readonly lineNormalOffset: WebGLUniformLocation
  private readonly lineColor: WebGLUniformLocation
  private readonly lineDashOn: WebGLUniformLocation
  private readonly lineDashPeriod: WebGLUniformLocation
  private readonly circleViewport: WebGLUniformLocation
  private readonly circleBuffer: WebGLBuffer
  private readonly circleVao: WebGLVertexArrayObject
  private readonly dynamicLineBuffer: WebGLBuffer
  private readonly dynamicLineVao: WebGLVertexArrayObject
  private readonly polylines = new WeakMap<TrackPoint[], CachedPolyline>()
  private readonly allocatedPolylines = new Set<CachedPolyline>()
  private circleData = new Float32Array(8 * 64)
  private circleCount = 0
  private readonly segmentData = new Float32Array(4 * 6)
  private width = 1
  private height = 1

  constructor(private readonly canvas: HTMLCanvasElement) {
    const gl = canvas.getContext('webgl2', {
      alpha: true,
      antialias: true,
      premultipliedAlpha: true,
      powerPreference: 'high-performance',
    })
    if (!gl) throw new Error('WebGL2 is unavailable')
    this.gl = gl
    this.lineProgram = program(gl, LINE_VERTEX, LINE_FRAGMENT)
    this.circleProgram = program(gl, CIRCLE_VERTEX, CIRCLE_FRAGMENT)

    this.lineViewport = requiredUniform(gl, this.lineProgram, 'uViewport')
    this.lineScale = requiredUniform(gl, this.lineProgram, 'uScale')
    this.lineOffset = requiredUniform(gl, this.lineProgram, 'uOffset')
    this.lineHalfWidth = requiredUniform(gl, this.lineProgram, 'uHalfWidth')
    this.lineNormalOffset = requiredUniform(gl, this.lineProgram, 'uNormalOffset')
    this.lineColor = requiredUniform(gl, this.lineProgram, 'uColor')
    this.lineDashOn = requiredUniform(gl, this.lineProgram, 'uDashOn')
    this.lineDashPeriod = requiredUniform(gl, this.lineProgram, 'uDashPeriod')
    this.circleViewport = requiredUniform(gl, this.circleProgram, 'uViewport')

    const circleBuffer = gl.createBuffer()
    const circleVao = gl.createVertexArray()
    const dynamicLineBuffer = gl.createBuffer()
    const dynamicLineVao = gl.createVertexArray()
    if (!circleBuffer || !circleVao || !dynamicLineBuffer || !dynamicLineVao) {
      throw new Error('Unable to create track-map WebGL buffers')
    }
    this.circleBuffer = circleBuffer
    this.circleVao = circleVao
    this.dynamicLineBuffer = dynamicLineBuffer
    this.dynamicLineVao = dynamicLineVao
    this.configureCircleVao(circleVao, circleBuffer)
    this.configureLineVao(dynamicLineVao, dynamicLineBuffer)
    gl.bindBuffer(gl.ARRAY_BUFFER, dynamicLineBuffer)
    gl.bufferData(gl.ARRAY_BUFFER, this.segmentData.byteLength, gl.DYNAMIC_DRAW)

    gl.enable(gl.BLEND)
    gl.blendFunc(gl.SRC_ALPHA, gl.ONE_MINUS_SRC_ALPHA)
    gl.disable(gl.DEPTH_TEST)
    gl.clearColor(0, 0, 0, 0)
  }

  resize(pixelWidth: number, pixelHeight: number): void {
    if (this.canvas.width !== pixelWidth) this.canvas.width = pixelWidth
    if (this.canvas.height !== pixelHeight) this.canvas.height = pixelHeight
    this.gl.viewport(0, 0, pixelWidth, pixelHeight)
  }

  beginFrame(width: number, height: number): void {
    this.width = Math.max(width, 1)
    this.height = Math.max(height, 1)
    this.circleCount = 0
    this.gl.clear(this.gl.COLOR_BUFFER_BIT)
  }

  drawPolyline(
    points: TrackPoint[],
    layout: TrackLayout,
    color: string,
    lineWidth: number,
    options: { alpha?: number; dashed?: boolean; dashSize?: number; normalOffset?: number } = {},
  ): void {
    if (points.length < 2 || lineWidth <= 0) return
    const geometry = this.polyline(points)
    this.drawLineGeometry(geometry.vao, geometry.vertices, layout, color, lineWidth, options)
  }

  drawScreenSegment(x0: number, y0: number, x1: number, y1: number, color: string, lineWidth: number): void {
    const length = Math.hypot(x1 - x0, y1 - y0)
    if (length <= 0 || lineWidth <= 0) return
    const nx = -(y1 - y0) / length
    const ny = (x1 - x0) / length
    this.putSegmentVertex(0, x0, y0, nx, ny, 0, -1)
    this.putSegmentVertex(6, x0, y0, nx, ny, 0, 1)
    this.putSegmentVertex(12, x1, y1, nx, ny, length, -1)
    this.putSegmentVertex(18, x1, y1, nx, ny, length, 1)
    const gl = this.gl
    gl.bindBuffer(gl.ARRAY_BUFFER, this.dynamicLineBuffer)
    gl.bufferSubData(gl.ARRAY_BUFFER, 0, this.segmentData)
    this.drawLineGeometry(this.dynamicLineVao, 4, { scale: 1, ox: 0, oy: 0 }, color, lineWidth)
  }

  queueCircle(cx: number, cy: number, radius: number, color: string, innerRatio = 0, alpha = 1): void {
    if (radius <= 0) return
    this.ensureCircleCapacity(this.circleCount + 1)
    const offset = this.circleCount * 8
    const rgb = parseColor(color)
    this.circleData[offset] = cx
    this.circleData[offset + 1] = cy
    this.circleData[offset + 2] = rgb[0]
    this.circleData[offset + 3] = rgb[1]
    this.circleData[offset + 4] = rgb[2]
    this.circleData[offset + 5] = alpha
    this.circleData[offset + 6] = radius
    this.circleData[offset + 7] = Math.max(0, Math.min(0.99, innerRatio))
    this.circleCount++
  }

  flushCircles(): void {
    if (this.circleCount === 0) return
    const gl = this.gl
    gl.useProgram(this.circleProgram)
    gl.uniform2f(this.circleViewport, this.width, this.height)
    gl.bindVertexArray(this.circleVao)
    gl.bindBuffer(gl.ARRAY_BUFFER, this.circleBuffer)
    gl.bufferData(gl.ARRAY_BUFFER, this.circleData.subarray(0, this.circleCount * 8), gl.DYNAMIC_DRAW)
    gl.drawArraysInstanced(gl.TRIANGLE_STRIP, 0, 4, this.circleCount)
    gl.bindVertexArray(null)
  }

  dispose(): void {
    const gl = this.gl
    for (const geometry of this.allocatedPolylines) {
      gl.deleteVertexArray(geometry.vao)
      gl.deleteBuffer(geometry.buffer)
    }
    this.allocatedPolylines.clear()
    gl.deleteVertexArray(this.circleVao)
    gl.deleteBuffer(this.circleBuffer)
    gl.deleteVertexArray(this.dynamicLineVao)
    gl.deleteBuffer(this.dynamicLineBuffer)
    gl.deleteProgram(this.lineProgram)
    gl.deleteProgram(this.circleProgram)
  }

  private drawLineGeometry(
    vao: WebGLVertexArrayObject,
    vertices: number,
    layout: TrackLayout,
    color: string,
    lineWidth: number,
    options: { alpha?: number; dashed?: boolean; dashSize?: number; normalOffset?: number } = {},
  ): void {
    const gl = this.gl
    const rgb = parseColor(color)
    const halfWidth = lineWidth / 2
    const dashSize = options.dashed ? (options.dashSize ?? 3) : 0
    gl.useProgram(this.lineProgram)
    gl.uniform2f(this.lineViewport, this.width, this.height)
    gl.uniform1f(this.lineScale, layout.scale)
    gl.uniform2f(this.lineOffset, layout.ox, layout.oy)
    gl.uniform1f(this.lineHalfWidth, halfWidth)
    gl.uniform1f(this.lineNormalOffset, options.normalOffset ?? 0)
    gl.uniform4f(this.lineColor, rgb[0], rgb[1], rgb[2], options.alpha ?? 1)
    gl.uniform1f(this.lineDashOn, dashSize)
    gl.uniform1f(this.lineDashPeriod, dashSize > 0 ? dashSize * 2 : 0)
    gl.bindVertexArray(vao)
    gl.drawArrays(gl.TRIANGLE_STRIP, 0, vertices)
    gl.bindVertexArray(null)
  }

  private polyline(points: TrackPoint[]): CachedPolyline {
    const existing = this.polylines.get(points)
    if (existing) return existing
    const gl = this.gl
    const data = new Float32Array(points.length * 2 * 6)
    let distance = 0
    for (let i = 0; i < points.length; i++) {
      if (i > 0) distance += Math.hypot(points[i][0] - points[i - 1][0], points[i][1] - points[i - 1][1])
      const point = points[i]
      const previous = points[Math.max(0, i - 1)]
      const next = points[Math.min(points.length - 1, i + 1)]
      let prevDx = point[0] - previous[0]
      let prevDy = point[1] - previous[1]
      let nextDx = next[0] - point[0]
      let nextDy = next[1] - point[1]
      let prevLength = Math.hypot(prevDx, prevDy)
      let nextLength = Math.hypot(nextDx, nextDy)
      if (prevLength <= 1e-9) {
        prevDx = nextDx
        prevDy = nextDy
        prevLength = nextLength
      }
      if (nextLength <= 1e-9) {
        nextDx = prevDx
        nextDy = prevDy
        nextLength = prevLength
      }
      prevLength ||= 1
      nextLength ||= 1
      const prevNx = -prevDy / prevLength
      const prevNy = prevDx / prevLength
      const nextNx = -nextDy / nextLength
      const nextNy = nextDx / nextLength
      const sumX = prevNx + nextNx
      const sumY = prevNy + nextNy
      const sumLength = Math.hypot(sumX, sumY)
      const miterX = sumLength > 1e-6 ? sumX / sumLength : nextNx
      const miterY = sumLength > 1e-6 ? sumY / sumLength : nextNy
      const denominator = Math.max(0.5, Math.abs(miterX * nextNx + miterY * nextNy))
      const miterScale = Math.min(2, 1 / denominator)
      const nx = miterX * miterScale
      const ny = miterY * miterScale
      for (let sideIndex = 0; sideIndex < 2; sideIndex++) {
        const offset = (i * 2 + sideIndex) * 6
        data[offset] = point[0]
        data[offset + 1] = point[1]
        data[offset + 2] = nx
        data[offset + 3] = ny
        data[offset + 4] = distance
        data[offset + 5] = sideIndex === 0 ? -1 : 1
      }
    }
    const buffer = gl.createBuffer()
    const vao = gl.createVertexArray()
    if (!buffer || !vao) throw new Error('Unable to cache track-map polyline')
    gl.bindBuffer(gl.ARRAY_BUFFER, buffer)
    gl.bufferData(gl.ARRAY_BUFFER, data, gl.STATIC_DRAW)
    this.configureLineVao(vao, buffer)
    const geometry = { buffer, vao, vertices: points.length * 2 }
    this.polylines.set(points, geometry)
    this.allocatedPolylines.add(geometry)
    return geometry
  }

  private configureLineVao(vao: WebGLVertexArrayObject, buffer: WebGLBuffer): void {
    const gl = this.gl
    gl.bindVertexArray(vao)
    gl.bindBuffer(gl.ARRAY_BUFFER, buffer)
    const stride = 6 * Float32Array.BYTES_PER_ELEMENT
    gl.enableVertexAttribArray(0)
    gl.vertexAttribPointer(0, 2, gl.FLOAT, false, stride, 0)
    gl.enableVertexAttribArray(1)
    gl.vertexAttribPointer(1, 2, gl.FLOAT, false, stride, 2 * Float32Array.BYTES_PER_ELEMENT)
    gl.enableVertexAttribArray(2)
    gl.vertexAttribPointer(2, 1, gl.FLOAT, false, stride, 4 * Float32Array.BYTES_PER_ELEMENT)
    gl.enableVertexAttribArray(3)
    gl.vertexAttribPointer(3, 1, gl.FLOAT, false, stride, 5 * Float32Array.BYTES_PER_ELEMENT)
    gl.bindVertexArray(null)
  }

  private configureCircleVao(vao: WebGLVertexArrayObject, buffer: WebGLBuffer): void {
    const gl = this.gl
    gl.bindVertexArray(vao)
    gl.bindBuffer(gl.ARRAY_BUFFER, buffer)
    const stride = 8 * Float32Array.BYTES_PER_ELEMENT
    gl.enableVertexAttribArray(0)
    gl.vertexAttribPointer(0, 2, gl.FLOAT, false, stride, 0)
    gl.vertexAttribDivisor(0, 1)
    gl.enableVertexAttribArray(1)
    gl.vertexAttribPointer(1, 4, gl.FLOAT, false, stride, 2 * Float32Array.BYTES_PER_ELEMENT)
    gl.vertexAttribDivisor(1, 1)
    gl.enableVertexAttribArray(2)
    gl.vertexAttribPointer(2, 1, gl.FLOAT, false, stride, 6 * Float32Array.BYTES_PER_ELEMENT)
    gl.vertexAttribDivisor(2, 1)
    gl.enableVertexAttribArray(3)
    gl.vertexAttribPointer(3, 1, gl.FLOAT, false, stride, 7 * Float32Array.BYTES_PER_ELEMENT)
    gl.vertexAttribDivisor(3, 1)
    gl.bindVertexArray(null)
  }

  private ensureCircleCapacity(count: number): void {
    if (count * 8 <= this.circleData.length) return
    const next = new Float32Array(Math.max(this.circleData.length * 2, count * 8))
    next.set(this.circleData)
    this.circleData = next
  }

  private putSegmentVertex(
    offset: number,
    x: number, y: number,
    nx: number, ny: number,
    distance: number,
    side: number,
  ): void {
    this.segmentData[offset] = x
    this.segmentData[offset + 1] = y
    this.segmentData[offset + 2] = nx
    this.segmentData[offset + 3] = ny
    this.segmentData[offset + 4] = distance
    this.segmentData[offset + 5] = side
  }
}
