import { DataPoint, RenderModel } from "../core/renderModel";
import { resolveColorRGBA, ResolvedCoreOptions, TimeChartSeriesOptions, LineType } from '../options';
import { domainSearch } from '../utils';
import { TimeChartPlugin } from '.';
import { LinkedWebGLProgram, throwIfFalsy } from './webGLUtils';
import { DataPointsBuffer } from "../core/dataPointsBuffer";


const BUFFER_TEXTURE_WIDTH = 256;
// A 256 x 256 RG32F page holds 65,536 points (512 KiB). Track N Race's normal
// ten-minute/60 Hz window is ~36,000 points, so one page covers the common case
// while SeriesVertexArray can still add overlapping pages for larger buffers.
const BUFFER_TEXTURE_HEIGHT = 256;
const BUFFER_POINT_CAPACITY = BUFFER_TEXTURE_WIDTH * BUFFER_TEXTURE_HEIGHT;
const BUFFER_INTERVAL_CAPACITY = BUFFER_POINT_CAPACITY - 2;
const dataPointX = (point: DataPoint) => point.x;

class ShaderUniformData {
    readonly data: ArrayBuffer;
    readonly ubo: WebGLBuffer;
    readonly modelScale: Float32Array;
    readonly modelTranslate: Float32Array;
    readonly projectionScale: Float32Array;

    constructor(private gl: WebGL2RenderingContext, size: number) {
        this.data = new ArrayBuffer(size);
        // These views target fixed std140 offsets and live for the lifetime of
        // the renderer. The upstream getters allocated three new views during
        // every draw.
        this.modelScale = new Float32Array(this.data, 0, 2);
        this.modelTranslate = new Float32Array(this.data, 2 * 4, 2);
        this.projectionScale = new Float32Array(this.data, 4 * 4, 2);
        this.ubo = throwIfFalsy(gl.createBuffer());
        gl.bindBuffer(gl.UNIFORM_BUFFER, this.ubo);
        gl.bufferData(gl.UNIFORM_BUFFER, this.data, gl.DYNAMIC_DRAW);
    }

    upload(index = 0) {
        this.gl.bindBufferBase(this.gl.UNIFORM_BUFFER, index, this.ubo);
        this.gl.bufferSubData(this.gl.UNIFORM_BUFFER, 0, this.data);
    }
}

const VS_HEADER = `#version 300 es
layout (std140) uniform proj {
    vec2 modelScale;
    vec2 modelTranslate;
    vec2 projectionScale;
};
uniform highp sampler2D uDataPoints;
uniform int uLineType;
uniform float uStepLocation;

const int TEX_WIDTH = ${BUFFER_TEXTURE_WIDTH};

vec2 dataPoint(int index) {
    int x = index % TEX_WIDTH;
    int y = index / TEX_WIDTH;
    return texelFetch(uDataPoints, ivec2(x, y), 0).xy;
}
`

const LINE_FS_SOURCE = `#version 300 es
precision lowp float;
uniform vec4 uColor;
out vec4 outColor;
void main() {
    outColor = uColor;
}`;

class NativeLineProgram extends LinkedWebGLProgram {
    locations;
    static VS_SOURCE = `${VS_HEADER}
uniform float uPointSize;

void main() {
    vec2 pos2d = projectionScale * modelScale * (dataPoint(gl_VertexID) + modelTranslate);
    gl_Position = vec4(pos2d, 0, 1);
    gl_PointSize = uPointSize;
}
`

    constructor(gl: WebGL2RenderingContext, debug: boolean) {
        super(gl, NativeLineProgram.VS_SOURCE, LINE_FS_SOURCE, debug);
        this.link();

        this.locations = {
            uDataPoints: this.getUniformLocation('uDataPoints'),
            uPointSize: this.getUniformLocation('uPointSize'),
            uColor: this.getUniformLocation('uColor'),
        }

        this.use();
        gl.uniform1i(this.locations.uDataPoints, 0);
        const projIdx = gl.getUniformBlockIndex(this.program, 'proj');
        gl.uniformBlockBinding(this.program, projIdx, 0);
    }
}

class LineProgram extends LinkedWebGLProgram {
    static VS_SOURCE = `${VS_HEADER}
uniform float uLineWidth;

void main() {
    int side = gl_VertexID & 1;
    int di = (gl_VertexID >> 1) & 1;
    int index = gl_VertexID >> 2;

    vec2 dp[2] = vec2[2](dataPoint(index), dataPoint(index + 1));

    vec2 base;
    vec2 off;
    if (uLineType == ${LineType.Line}) {
        base = dp[di];
        vec2 dir = dp[1] - dp[0];
        dir = normalize(modelScale * dir);
        off = vec2(-dir.y, dir.x) * uLineWidth;
    } else if (uLineType == ${LineType.Step}) {
        base = vec2(dp[0].x * (1. - uStepLocation) + dp[1].x * uStepLocation, dp[di].y);
        float up = sign(dp[0].y - dp[1].y);
        off = vec2(uLineWidth * up, uLineWidth);
    }

    if (side == 1)
        off = -off;
    vec2 cssPose = modelScale * (base + modelTranslate);
    vec2 pos2d = projectionScale * (cssPose + off);
    gl_Position = vec4(pos2d, 0, 1);
}`;

    locations;
    constructor(gl: WebGL2RenderingContext, debug: boolean) {
        super(gl, LineProgram.VS_SOURCE, LINE_FS_SOURCE, debug);
        this.link();

        this.locations = {
            uDataPoints: this.getUniformLocation('uDataPoints'),
            uLineType: this.getUniformLocation('uLineType'),
            uStepLocation: this.getUniformLocation('uStepLocation'),
            uLineWidth: this.getUniformLocation('uLineWidth'),
            uColor: this.getUniformLocation('uColor'),
        }

        this.use();
        gl.uniform1i(this.locations.uDataPoints, 0);
        const projIdx = gl.getUniformBlockIndex(this.program, 'proj');
        gl.uniformBlockBinding(this.program, projIdx, 0);
    }
}

class SeriesSegmentVertexArray {
    readonly dataBuffer: WebGLTexture;
    // One texture-row staging area, retained for the segment lifetime. Live
    // updates use only its first few floats for the point and endpoint padding.
    private readonly uploadBuffer = new Float32Array(BUFFER_TEXTURE_WIDTH * 2);

    constructor(
        private gl: WebGL2RenderingContext,
        private dataPoints: DataPointsBuffer,
    ) {
        this.dataBuffer = throwIfFalsy(gl.createTexture());
        gl.bindTexture(gl.TEXTURE_2D, this.dataBuffer);
        gl.texStorage2D(gl.TEXTURE_2D, 1, gl.RG32F, BUFFER_TEXTURE_WIDTH, BUFFER_TEXTURE_HEIGHT);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
    }

    delete() {
        this.gl.deleteTexture(this.dataBuffer);
    }

    syncPoints(start: number, n: number, bufferPos: number) {
        const dps = this.dataPoints;
        // Upload only the changed linear span. Keep one repeated endpoint texel
        // on either side when this span touches the beginning/end of the data;
        // the line shaders can read that neighbour at segment boundaries.
        let textureStart = bufferPos;
        let textureEnd = bufferPos + n;
        if (start === 0 && textureStart > 0) textureStart--;
        if (start + n === dps.length && textureEnd < BUFFER_POINT_CAPACITY) textureEnd++;
        if (textureStart === textureEnd) return;

        const gl = this.gl;
        gl.bindTexture(gl.TEXTURE_2D, this.dataBuffer);
        while (textureStart < textureEnd) {
            const xOffset = textureStart % BUFFER_TEXTURE_WIDTH;
            const yOffset = Math.floor(textureStart / BUFFER_TEXTURE_WIDTH);
            const count = Math.min(textureEnd - textureStart, BUFFER_TEXTURE_WIDTH - xOffset);
            for (let i = 0; i < count; i++) {
                const texturePos = textureStart + i;
                const dataIndex = Math.max(Math.min(start + texturePos - bufferPos, dps.length - 1), 0);
                const point = dps[dataIndex];
                this.uploadBuffer[i * 2] = point.x;
                this.uploadBuffer[i * 2 + 1] = point.y;
            }
            // WebGL consumes exactly width * height * 2 floats; the unused tail
            // of the retained row buffer is ignored.
            gl.texSubImage2D(
                gl.TEXTURE_2D, 0, xOffset, yOffset, count, 1,
                gl.RG, gl.FLOAT, this.uploadBuffer,
            );
            textureStart += count;
        }
    }

    /**
     * @param renderInterval [start, end) interval of data points, start from 0
     */
    draw(renderStart: number, renderEnd: number, type: LineType) {
        const first = Math.max(0, renderStart);
        const last = Math.min(BUFFER_INTERVAL_CAPACITY, renderEnd)
        const count = last - first

        const gl = this.gl;
        gl.activeTexture(gl.TEXTURE0);
        gl.bindTexture(gl.TEXTURE_2D, this.dataBuffer);
        if (type === LineType.Line) {
            gl.drawArrays(gl.TRIANGLE_STRIP, first * 4, count * 4 + (last !== renderEnd ? 2 : 0));
        } else if (type === LineType.Step) {
            let firstP = first * 4;
            let countP = count * 4 + 2;
            if (first === renderStart) {
                firstP -= 2;
                countP += 2;
            }
            gl.drawArrays(gl.TRIANGLE_STRIP, firstP, countP);
        } else if (type === LineType.NativeLine) {
            gl.drawArrays(gl.LINE_STRIP, first, count + 1);
        } else if (type === LineType.NativePoint) {
            gl.drawArrays(gl.POINTS, first, count + 1);
        }
    }
}

/**
 * An array of `SeriesSegmentVertexArray` to represent a series
 */
class SeriesVertexArray {
    private segments = [] as SeriesSegmentVertexArray[];
    // each segment has at least 2 points
    private validStart = 0;  // start position of the first segment. (0, BUFFER_INTERVAL_CAPACITY]
    private validEnd = 0;    // end position of the last segment. [2, BUFFER_POINT_CAPACITY)

    constructor(
        private gl: WebGL2RenderingContext,
        private series: TimeChartSeriesOptions,
    ) {
    }

    private popFront() {
        if (this.series.data.poped_front === 0)
            return;

        this.validStart += this.series.data.poped_front;

        while (this.validStart > BUFFER_INTERVAL_CAPACITY) {
            const activeArray = this.segments[0];
            activeArray.delete();
            this.segments.shift();
            this.validStart -= BUFFER_INTERVAL_CAPACITY;
        }

        this.segments[0].syncPoints(0, 0, this.validStart);
    }
    private popBack() {
        if (this.series.data.poped_back === 0)
            return;

        this.validEnd -= this.series.data.poped_back;

        while (this.validEnd < BUFFER_POINT_CAPACITY - BUFFER_INTERVAL_CAPACITY) {
            const activeArray = this.segments[this.segments.length - 1];
            activeArray.delete();
            this.segments.pop();
            this.validEnd += BUFFER_INTERVAL_CAPACITY;
        }

        this.segments[this.segments.length - 1].syncPoints(this.series.data.length, 0, this.validEnd);
    }

    private newArray() {
        return new SeriesSegmentVertexArray(this.gl, this.series.data);
    }
    private pushFront() {
        let numDPtoAdd = this.series.data.pushed_front;
        if (numDPtoAdd === 0)
            return;

        const newArray = () => {
            this.segments.unshift(this.newArray());
            this.validStart = BUFFER_POINT_CAPACITY;
        }

        if (this.segments.length === 0) {
            newArray();
            this.validEnd = this.validStart = BUFFER_POINT_CAPACITY - 1;
        }

        while (true) {
            const activeArray = this.segments[0];
            const n = Math.min(this.validStart, numDPtoAdd);
            activeArray.syncPoints(numDPtoAdd - n, n, this.validStart - n);
            numDPtoAdd -= this.validStart - (BUFFER_POINT_CAPACITY - BUFFER_INTERVAL_CAPACITY);
            this.validStart -= n;
            if (this.validStart > 0)
                break;
            newArray();
        }
    }

    private pushBack() {
        let numDPtoAdd = this.series.data.pushed_back;
        if (numDPtoAdd === 0)
            return

        const newArray = () => {
            this.segments.push(this.newArray());
            this.validEnd = 0;
        }

        if (this.segments.length === 0) {
            newArray();
            this.validEnd = this.validStart = 1;
        }

        while (true) {
            const activeArray = this.segments[this.segments.length - 1];
            const n = Math.min(BUFFER_POINT_CAPACITY - this.validEnd, numDPtoAdd);
            activeArray.syncPoints(this.series.data.length - numDPtoAdd, n, this.validEnd);
            // Note that each segment overlaps with the previous one.
            // numDPtoAdd can increase here, indicating the overlapping part should be synced again to the next segment
            numDPtoAdd -= BUFFER_INTERVAL_CAPACITY - this.validEnd;
            this.validEnd += n;
            // Fully fill the previous segment before creating a new one
            if (this.validEnd < BUFFER_POINT_CAPACITY)
                break;
            newArray();
        }
    }

    deinit() {
        for (const s of this.segments)
            s.delete();
        this.segments = [];
    }

    syncBuffer() {
        const d = this.series.data;
        if (d.length - d.pushed_back - d.pushed_front < 2) {
            this.deinit();
            d.poped_front = d.poped_back = 0;
        }
        if (this.segments.length === 0) {
            if (d.length >= 2) {
                if (d.pushed_back > d.pushed_front) {
                    d.pushed_back = d.length;
                    this.pushBack();
                } else {
                    d.pushed_front = d.length;
                    this.pushFront();
                }
            }
            return;
        }
        this.popFront();
        this.popBack();
        this.pushFront();
        this.pushBack();
    }

    draw(renderMin: number, renderMax: number) {
        const data = this.series.data;
        if (this.segments.length === 0 || data[0].x > renderMax || data[data.length - 1].x < renderMin)
            return;

        const firstDP = domainSearch(data, 1, data.length, renderMin, dataPointX) - 1;
        const lastDP = domainSearch(data, firstDP, data.length - 1, renderMax, dataPointX)
        const startInterval = firstDP + this.validStart;
        const endInterval = lastDP + this.validStart;
        const startArray = Math.floor(startInterval / BUFFER_INTERVAL_CAPACITY);
        const endArray = Math.ceil(endInterval / BUFFER_INTERVAL_CAPACITY);

        for (let i = startArray; i < endArray; i++) {
            const arrOffset = i * BUFFER_INTERVAL_CAPACITY
            this.segments[i].draw(
                startInterval - arrOffset,
                endInterval - arrOffset,
                this.series.lineType,
            );
        }
    }
}

export class LineChartRenderer {
    private lineProgram = new LineProgram(this.gl, this.options.debugWebGL);
    private nativeLineProgram = new NativeLineProgram(this.gl, this.options.debugWebGL);
    private uniformBuffer: ShaderUniformData;
    private arrays = new Map<TimeChartSeriesOptions, SeriesVertexArray>();
    private height = 0;
    private width = 0;
    private renderHeight = 0;
    private renderWidth = 0;
    private xRangeStart = 0;
    private xRangeEnd = 0;
    private yRangeStart = 0;
    private yRangeEnd = 0;
    private xDomainMin = 0;
    private xUnitsPerPixel = 1;
    private colorCache = new Map<TimeChartSeriesOptions, {
        source: ResolvedCoreOptions['color'] | TimeChartSeriesOptions['color'];
        rgba: ReturnType<typeof resolveColorRGBA>;
    }>();

    constructor(
        private model: RenderModel,
        private gl: WebGL2RenderingContext,
        private options: ResolvedCoreOptions,
    ) {
        const uboSize = gl.getActiveUniformBlockParameter(this.lineProgram.program, 0, gl.UNIFORM_BLOCK_DATA_SIZE);
        this.uniformBuffer = new ShaderUniformData(this.gl, uboSize);

        model.updated.on(() => this.drawFrame());
        model.resized.on((w, h) => this.onResize(w, h));
    }

    syncBuffer() {
        for (const s of this.options.series) {
            let a = this.arrays.get(s);
            if (!a) {
                a = new SeriesVertexArray(this.gl, s);
                this.arrays.set(s, a);
            }
            a.syncBuffer();
        }
    }

    syncViewport() {
        this.renderWidth = this.width - this.options.renderPaddingLeft - this.options.renderPaddingRight;
        this.renderHeight = this.height - this.options.renderPaddingTop - this.options.renderPaddingBottom;
        const projection = this.uniformBuffer.projectionScale;
        projection[0] = 2 / this.renderWidth;
        projection[1] = 2 / this.renderHeight;
    }

    onResize(width: number, height: number) {
        this.height = height;
        this.width = width;
        // Projection geometry and scale ranges are resize-invariant. Keep them
        // out of the 60 Hz draw path.
        const xRange = this.model.xScale.range();
        const yRange = this.model.yScale.range();
        this.xRangeStart = Number(xRange[0]);
        this.xRangeEnd = Number(xRange[1]);
        this.yRangeStart = Number(yRange[0]);
        this.yRangeEnd = Number(yRange[1]);
        this.syncViewport();
    }

    private colorFor(series: TimeChartSeriesOptions) {
        const source = series.color ?? this.options.color;
        const cached = this.colorCache.get(series);
        if (cached?.source === source) return cached.rgba;
        const rgba = resolveColorRGBA(source);
        this.colorCache.set(series, { source, rgba });
        return rgba;
    }

    drawFrame() {
        this.syncBuffer();
        this.syncDomain();
        this.uniformBuffer.upload();
        const gl = this.gl;
        let activeProgram: LineProgram | NativeLineProgram | null = null;
        for (const [ds, arr] of this.arrays) {
            if (!ds.visible) {
                continue;
            }

            const prog = ds.lineType === LineType.NativeLine || ds.lineType === LineType.NativePoint ? this.nativeLineProgram : this.lineProgram;
            if (prog !== activeProgram) {
                prog.use();
                activeProgram = prog;
            }
            gl.uniform4fv(prog.locations.uColor, this.colorFor(ds));

            const lineWidth = ds.lineWidth ?? this.options.lineWidth;
            if (prog instanceof LineProgram) {
                gl.uniform1i(prog.locations.uLineType, ds.lineType);
                gl.uniform1f(prog.locations.uLineWidth, lineWidth / 2);
                if (ds.lineType === LineType.Step)
                    gl.uniform1f(prog.locations.uStepLocation, ds.stepLocation);
            } else {
                if (ds.lineType === LineType.NativeLine)
                    gl.lineWidth(lineWidth * this.options.pixelRatio);  // Not working on most platforms
                else if (ds.lineType === LineType.NativePoint)
                    gl.uniform1f(prog.locations.uPointSize, lineWidth * this.options.pixelRatio);
            }

            const renderMin = this.xDomainMin +
                (this.options.renderPaddingLeft - lineWidth / 2 - this.xRangeStart) * this.xUnitsPerPixel;
            const renderMax = this.xDomainMin +
                (this.width - this.options.renderPaddingRight + lineWidth / 2 - this.xRangeStart) * this.xUnitsPerPixel;
            arr.draw(renderMin, renderMax);
        }
        if (this.options.debugWebGL) {
            const err = gl.getError();
            if (err != gl.NO_ERROR) {
                throw new Error(`WebGL error ${err}`);
            }
        }
    }

    syncDomain() {
        const m = this.model;

        // for any x,
        // (x - domain[0]) / (domain[1] - domain[0]) * (range[1] - range[0]) + range[0] - W / 2 - padding = s * (x + t)
        // => s = (range[1] - range[0]) / (domain[1] - domain[0])
        //    t = (range[0] - W / 2 - padding) / s - domain[0]

        // Scalar math preserves precision and avoids temporary vectors/arrays.
        let xMin: number;
        let xMax: number;
        const configuredX = this.options.xRange;
        if (!this.options.realTime && configuredX && configuredX !== 'auto') {
            xMin = Number(configuredX.min);
            xMax = Number(configuredX.max);
        } else {
            const domain = m.xScale.domain();
            xMin = Number(domain[0]);
            xMax = Number(domain[1]);
        }

        let yMin: number;
        let yMax: number;
        const configuredY = this.options.yRange;
        if (configuredY && configuredY !== 'auto') {
            yMin = configuredY.min;
            yMax = configuredY.max;
        } else {
            const domain = m.yScale.domain();
            yMin = Number(domain[0]);
            yMax = Number(domain[1]);
        }

        const sx = (this.xRangeEnd - this.xRangeStart) / (xMax - xMin);
        const sy = (this.yRangeStart - this.yRangeEnd) / (yMax - yMin);
        const uniforms = this.uniformBuffer;
        uniforms.modelScale[0] = sx;
        uniforms.modelScale[1] = sy;
        uniforms.modelTranslate[0] =
            (this.xRangeStart - this.renderWidth / 2 - this.options.renderPaddingLeft) / sx - xMin;
        uniforms.modelTranslate[1] =
            -(this.yRangeStart - this.renderHeight / 2 - this.options.renderPaddingTop) / sy - yMin;

        this.xDomainMin = xMin;
        this.xUnitsPerPixel = 1 / sx;
    }
}

export const lineChart: TimeChartPlugin<LineChartRenderer> = {
    apply(chart) {
        return new LineChartRenderer(chart.model, chart.canvasLayer.gl, chart.options);
    }
}
