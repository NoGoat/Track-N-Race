import { RenderModel } from '../core/renderModel';
import {
    ALIGNED_PAGE_COUNT,
    ALIGNED_PAGE_SIZE,
    ALIGNED_PHYSICAL_CAPACITY,
    AlignedDataBuffer,
} from '../core/alignedData';
import { resolveColorRGBA, ResolvedCoreOptions, TimeChartSeriesOptions, LineType } from '../options';
import { TimeChartPlugin } from '.';
import { LinkedWebGLProgram, throwIfFalsy } from './webGLUtils';

const TEXTURE_WIDTH = 256;
const DATA_OFFSET = 0;
const TAIL_PADDING = 1;
const TEXTURE_HEIGHT = Math.ceil((DATA_OFFSET + ALIGNED_PAGE_SIZE + TAIL_PADDING) / TEXTURE_WIDTH);

class ShaderUniformData {
    readonly data: ArrayBuffer;
    readonly ubo: WebGLBuffer;
    readonly modelScale: Float32Array;
    readonly modelTranslate: Float32Array;
    readonly projectionScale: Float32Array;

    constructor(private gl: WebGL2RenderingContext, size: number) {
        this.data = new ArrayBuffer(size);
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

    delete() {
        this.gl.deleteBuffer(this.ubo);
    }
}

const VS_HEADER = `#version 300 es
layout (std140) uniform proj {
    vec2 modelScale;
    vec2 modelTranslate;
    vec2 projectionScale;
};
uniform highp sampler2D uXPoints;
uniform highp sampler2DArray uYPoints;
uniform int uYChannel;
uniform int uLineType;
uniform float uStepLocation;

const int TEX_WIDTH = ${TEXTURE_WIDTH};

ivec2 texturePosition(int index) {
    return ivec2(index % TEX_WIDTH, index / TEX_WIDTH);
}

vec2 dataPoint(int index) {
    ivec2 pos = texturePosition(index);
    float x = texelFetch(uXPoints, pos, 0).r;
    float y = texelFetch(uYPoints, ivec3(pos, uYChannel), 0).r;
    return vec2(x, y);
}
`;

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
`;

    constructor(gl: WebGL2RenderingContext, debug: boolean) {
        super(gl, NativeLineProgram.VS_SOURCE, LINE_FS_SOURCE, debug);
        this.link();
        this.locations = {
            uXPoints: this.getUniformLocation('uXPoints'),
            uYPoints: this.getUniformLocation('uYPoints'),
            uYChannel: this.getUniformLocation('uYChannel'),
            uPointSize: this.getUniformLocation('uPointSize'),
            uColor: this.getUniformLocation('uColor'),
        };
        this.use();
        gl.uniform1i(this.locations.uXPoints, 0);
        gl.uniform1i(this.locations.uYPoints, 1);
        const projIdx = gl.getUniformBlockIndex(this.program, 'proj');
        gl.uniformBlockBinding(this.program, projIdx, 0);
    }
}

class LineProgram extends LinkedWebGLProgram {
    static VS_SOURCE = `${VS_HEADER}
uniform float uLineWidth;
uniform int uStepSegments;

vec2 stepPoint(int virtualIndex) {
    int interval = virtualIndex / uStepSegments;
    int part = virtualIndex - interval * uStepSegments;
    vec2 p0 = dataPoint(interval);
    if (part == 0) return p0;
    vec2 p1 = dataPoint(interval + 1);
    float transitionX = mix(p0.x, p1.x, uStepLocation);
    if (uStepSegments == 2) {
        return uStepLocation <= .5 ? vec2(transitionX, p1.y) : vec2(transitionX, p0.y);
    }
    return part == 1 ? vec2(transitionX, p0.y) : vec2(transitionX, p1.y);
}

void main() {
    int side = gl_VertexID & 1;
    int di = (gl_VertexID >> 1) & 1;
    int index = gl_VertexID >> 2;

    vec2 dp[2];
    if (uLineType == ${LineType.Line}) {
        dp[0] = dataPoint(index);
        dp[1] = dataPoint(index + 1);
    } else {
        dp[0] = stepPoint(index);
        dp[1] = stepPoint(index + 1);
    }
    vec2 base = dp[di];
    vec2 scaledDirection = modelScale * (dp[1] - dp[0]);
    float directionLength = length(scaledDirection);
    vec2 dir = directionLength > 0. ? scaledDirection / directionLength : vec2(0.);
    vec2 off = vec2(-dir.y, dir.x) * uLineWidth;

    if (side == 1) off = -off;
    vec2 cssPose = modelScale * (base + modelTranslate);
    gl_Position = vec4(projectionScale * (cssPose + off), 0, 1);
}`;

    locations;
    constructor(gl: WebGL2RenderingContext, debug: boolean) {
        super(gl, LineProgram.VS_SOURCE, LINE_FS_SOURCE, debug);
        this.link();
        this.locations = {
            uXPoints: this.getUniformLocation('uXPoints'),
            uYPoints: this.getUniformLocation('uYPoints'),
            uYChannel: this.getUniformLocation('uYChannel'),
            uLineType: this.getUniformLocation('uLineType'),
            uStepLocation: this.getUniformLocation('uStepLocation'),
            uStepSegments: this.getUniformLocation('uStepSegments'),
            uLineWidth: this.getUniformLocation('uLineWidth'),
            uColor: this.getUniformLocation('uColor'),
        };
        this.use();
        gl.uniform1i(this.locations.uXPoints, 0);
        gl.uniform1i(this.locations.uYPoints, 1);
        gl.uniform1f(this.locations.uStepLocation, 1);
        gl.uniform1i(this.locations.uStepSegments, 2);
        const projIdx = gl.getUniformBlockIndex(this.program, 'proj');
        gl.uniformBlockBinding(this.program, projIdx, 0);
    }
}

// Filled areas use the same resident paged textures as their line. Generating
// baseline/sample vertex pairs in the shader avoids rebuilding a CPU canvas
// path from the full All Laps history on every frame.
class AreaProgram extends LinkedWebGLProgram {
    static VS_SOURCE = `${VS_HEADER}
uniform float uBaseline;
uniform int uStepSegments;

vec2 stepPoint(int virtualIndex) {
    int interval = virtualIndex / uStepSegments;
    int part = virtualIndex - interval * uStepSegments;
    vec2 p0 = dataPoint(interval);
    if (part == 0) return p0;
    vec2 p1 = dataPoint(interval + 1);
    float transitionX = mix(p0.x, p1.x, uStepLocation);
    if (uStepSegments == 2) {
        return uStepLocation <= .5 ? vec2(transitionX, p1.y) : vec2(transitionX, p0.y);
    }
    return part == 1 ? vec2(transitionX, p0.y) : vec2(transitionX, p1.y);
}

void main() {
    int pathIndex = gl_VertexID >> 1;
    vec2 top = uLineType == ${LineType.Step} ? stepPoint(pathIndex) : dataPoint(pathIndex);
    vec2 point = (gl_VertexID & 1) == 0 ? vec2(top.x, uBaseline) : top;
    vec2 cssPose = modelScale * (point + modelTranslate);
    gl_Position = vec4(projectionScale * cssPose, 0, 1);
}`;

    locations;
    constructor(gl: WebGL2RenderingContext, debug: boolean) {
        super(gl, AreaProgram.VS_SOURCE, LINE_FS_SOURCE, debug);
        this.link();
        this.locations = {
            uXPoints: this.getUniformLocation('uXPoints'),
            uYPoints: this.getUniformLocation('uYPoints'),
            uYChannel: this.getUniformLocation('uYChannel'),
            uLineType: this.getUniformLocation('uLineType'),
            uStepLocation: this.getUniformLocation('uStepLocation'),
            uStepSegments: this.getUniformLocation('uStepSegments'),
            uBaseline: this.getUniformLocation('uBaseline'),
            uColor: this.getUniformLocation('uColor'),
        };
        this.use();
        gl.uniform1i(this.locations.uXPoints, 0);
        gl.uniform1i(this.locations.uYPoints, 1);
        gl.uniform1f(this.locations.uStepLocation, 1);
        gl.uniform1i(this.locations.uStepSegments, 2);
        const projIdx = gl.getUniformBlockIndex(this.program, 'proj');
        gl.uniformBlockBinding(this.program, projIdx, 0);
    }
}

/** One lazily allocated physical ring page shared by every series channel. */
class SharedGpuPage {
    readonly xTexture: WebGLTexture;
    readonly yTexture: WebGLTexture;

    constructor(private gl: WebGL2RenderingContext, channelCount: number) {
        this.xTexture = throwIfFalsy(gl.createTexture());
        gl.bindTexture(gl.TEXTURE_2D, this.xTexture);
        gl.texStorage2D(gl.TEXTURE_2D, 1, gl.R32F, TEXTURE_WIDTH, TEXTURE_HEIGHT);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);

        this.yTexture = throwIfFalsy(gl.createTexture());
        gl.bindTexture(gl.TEXTURE_2D_ARRAY, this.yTexture);
        gl.texStorage3D(gl.TEXTURE_2D_ARRAY, 1, gl.R32F, TEXTURE_WIDTH, TEXTURE_HEIGHT, channelCount);
        gl.texParameteri(gl.TEXTURE_2D_ARRAY, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
        gl.texParameteri(gl.TEXTURE_2D_ARRAY, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
    }

    bind() {
        const gl = this.gl;
        gl.activeTexture(gl.TEXTURE0);
        gl.bindTexture(gl.TEXTURE_2D, this.xTexture);
        gl.activeTexture(gl.TEXTURE1);
        gl.bindTexture(gl.TEXTURE_2D_ARRAY, this.yTexture);
    }

    delete() {
        this.gl.deleteTexture(this.xTexture);
        this.gl.deleteTexture(this.yTexture);
    }
}

/** Mirrors one AlignedDataBuffer without duplicating its X values per series. */
class SharedGpuBuffer {
    private readonly pages: Array<SharedGpuPage | undefined> = new Array(ALIGNED_PAGE_COUNT);
    private readonly xUpload = new Float32Array(TEXTURE_WIDTH);
    private readonly pointUpload = new Float32Array(1);

    constructor(
        private gl: WebGL2RenderingContext,
        readonly data: AlignedDataBuffer,
    ) {}

    private ensurePage(pageIndex: number) {
        let page = this.pages[pageIndex];
        if (!page) {
            page = new SharedGpuPage(this.gl, this.data.channelCount);
            this.pages[pageIndex] = page;
        }
        return page;
    }

    private deletePage(pageIndex: number) {
        this.pages[pageIndex]?.delete();
        this.pages[pageIndex] = undefined;
    }

    private uploadChunk(pageIndex: number, pageOffset: number, count: number) {
        const xSource = this.data.xPage(pageIndex);
        if (!xSource) return;
        const page = this.ensurePage(pageIndex);
        const textureOffset = pageOffset + DATA_OFFSET;
        const x = textureOffset % TEXTURE_WIDTH;
        const y = Math.floor(textureOffset / TEXTURE_WIDTH);
        const gl = this.gl;

        for (let i = 0; i < count; i++) this.xUpload[i] = xSource[pageOffset + i];
        gl.bindTexture(gl.TEXTURE_2D, page.xTexture);
        gl.texSubImage2D(gl.TEXTURE_2D, 0, x, y, count, 1, gl.RED, gl.FLOAT, this.xUpload);

        gl.bindTexture(gl.TEXTURE_2D_ARRAY, page.yTexture);
        for (let channel = 0; channel < this.data.channelCount; channel++) {
            const ySource = this.data.yPage(channel, pageIndex)!;
            gl.texSubImage3D(
                gl.TEXTURE_2D_ARRAY, 0, x, y, channel, count, 1, 1,
                gl.RED, gl.FLOAT, ySource, pageOffset,
            );
        }
    }

    private uploadSpan(physicalStart: number, count: number) {
        let physical = physicalStart;
        let remaining = count;
        while (remaining > 0) {
            const pageIndex = Math.floor(physical / ALIGNED_PAGE_SIZE);
            const pageOffset = physical % ALIGNED_PAGE_SIZE;
            const rowRemaining = TEXTURE_WIDTH - ((pageOffset + DATA_OFFSET) % TEXTURE_WIDTH);
            const chunk = Math.min(remaining, ALIGNED_PAGE_SIZE - pageOffset, rowRemaining);
            this.uploadChunk(pageIndex, pageOffset, chunk);
            physical = (physical + chunk) % ALIGNED_PHYSICAL_CAPACITY;
            remaining -= chunk;
        }
    }

    private uploadPaddingPoint(pageIndex: number, textureOffset: number, logicalIndex: number) {
        const page = this.pages[pageIndex];
        if (!page || logicalIndex < 0) return;
        const gl = this.gl;
        const x = textureOffset % TEXTURE_WIDTH;
        const y = Math.floor(textureOffset / TEXTURE_WIDTH);

        this.xUpload[0] = this.data.xAt(logicalIndex);
        gl.bindTexture(gl.TEXTURE_2D, page.xTexture);
        gl.texSubImage2D(gl.TEXTURE_2D, 0, x, y, 1, 1, gl.RED, gl.FLOAT, this.xUpload);
        gl.bindTexture(gl.TEXTURE_2D_ARRAY, page.yTexture);
        for (let channel = 0; channel < this.data.channelCount; channel++) {
            this.pointUpload[0] = this.data.yAt(channel, logicalIndex);
            gl.texSubImage3D(
                gl.TEXTURE_2D_ARRAY, 0, x, y, channel, 1, 1, 1,
                gl.RED, gl.FLOAT, this.pointUpload,
            );
        }
    }

    /** Maintain the one neighbour texel used by every page-edge segment. */
    private refreshPadding(pageIndex: number) {
        if (!this.pages[pageIndex]) return;
        const pageStart = pageIndex * ALIGNED_PAGE_SIZE;
        const pageLast = pageStart + ALIGNED_PAGE_SIZE - 1;
        const last = this.data.logicalIndexForPhysical(pageLast);
        if (last < 0) return;
        const nextPhysical = (pageLast + 1) % ALIGNED_PHYSICAL_CAPACITY;
        const next = this.data.logicalIndexForPhysical(nextPhysical);
        this.uploadPaddingPoint(pageIndex, DATA_OFFSET + ALIGNED_PAGE_SIZE, next >= 0 ? next : last);
    }

    sync() {
        const reset = this.data.resetPending;
        const appended = this.data.pushedBack;
        const evicted = this.data.poppedFront;
        // Static All Laps frames still redraw at display rate, but their GPU
        // pages are already resident. Avoid even walking the page directory
        // unless the aligned ring reports an actual mutation.
        if (!reset && appended === 0 && evicted === 0) return;
        const dirtySpans = appended === 0 ? [] : this.data.dirtySpans();
        if (reset) {
            for (let page = 0; page < ALIGNED_PAGE_COUNT; page++) this.deletePage(page);
        } else if (evicted !== 0) {
            for (let page = 0; page < ALIGNED_PAGE_COUNT; page++) {
                if (!this.data.hasPage(page)) this.deletePage(page);
            }
        }

        for (const span of dirtySpans) this.uploadSpan(span.start, span.count);
        if (reset) {
            for (let page = 0; page < ALIGNED_PAGE_COUNT; page++) {
                if (this.data.hasPage(page)) this.refreshPadding(page);
            }
        } else {
            // Only a write beginning at a physical page boundary changes the
            // neighbour texel stored at the end of the preceding page. Normal
            // appends within a page need no padding upload at all.
            for (const span of dirtySpans) {
                let physical = span.start;
                let remaining = span.count;
                while (remaining > 0) {
                    const page = Math.floor(physical / ALIGNED_PAGE_SIZE);
                    const offset = physical % ALIGNED_PAGE_SIZE;
                    const chunk = Math.min(remaining, ALIGNED_PAGE_SIZE - offset);
                    if (offset === 0) {
                        const previous = (page + ALIGNED_PAGE_COUNT - 1) % ALIGNED_PAGE_COUNT;
                        if (this.data.hasPage(previous)) this.refreshPadding(previous);
                    }
                    physical = (physical + chunk) % ALIGNED_PHYSICAL_CAPACITY;
                    remaining -= chunk;
                }
            }
        }
    }

    drawPage(
        pageIndex: number,
        channel: number,
        firstPoint: number,
        intervalCount: number,
        type: LineType,
        stepSegments: number,
        program: LineProgram | NativeLineProgram,
    ) {
        const page = this.pages[pageIndex];
        if (!page || intervalCount <= 0) return;
        page.bind();
        this.gl.uniform1i(program.locations.uYChannel, channel);
        if (type === LineType.Line) {
            this.gl.drawArrays(this.gl.TRIANGLE_STRIP, firstPoint * 4, intervalCount * 4);
        } else if (type === LineType.Step) {
            this.gl.drawArrays(
                this.gl.TRIANGLE_STRIP,
                firstPoint * stepSegments * 4,
                intervalCount * stepSegments * 4,
            );
        } else if (type === LineType.NativeLine) {
            this.gl.drawArrays(this.gl.LINE_STRIP, firstPoint, intervalCount + 1);
        } else {
            this.gl.drawArrays(this.gl.POINTS, firstPoint, intervalCount + 1);
        }
    }

    drawAreaPage(
        pageIndex: number,
        channel: number,
        firstPoint: number,
        intervalCount: number,
        type: LineType,
        stepSegments: number,
        program: AreaProgram,
    ) {
        const page = this.pages[pageIndex];
        if (!page || intervalCount <= 0) return;
        page.bind();
        this.gl.uniform1i(program.locations.uYChannel, channel);
        if (type === LineType.Step) {
            this.gl.drawArrays(
                this.gl.TRIANGLE_STRIP,
                firstPoint * stepSegments * 2,
                (intervalCount * stepSegments + 1) * 2,
            );
        } else {
            this.gl.drawArrays(this.gl.TRIANGLE_STRIP, firstPoint * 2, (intervalCount + 1) * 2);
        }
    }

    delete() {
        for (let page = 0; page < ALIGNED_PAGE_COUNT; page++) this.deletePage(page);
    }
}

class SeriesGpuView {
    constructor(
        private gpu: SharedGpuBuffer,
        private series: TimeChartSeriesOptions,
    ) {}

    draw(renderMin: number, renderMax: number, program: LineProgram | NativeLineProgram) {
        const data = this.series.data;
        if (data.length < 2 || data.xAt(0) > renderMax || data.xAt(data.length - 1) < renderMin) return;

        let firstInterval = data.lowerBoundX(renderMin, 1, data.length) - 1;
        const endInterval = data.lowerBoundX(renderMax, firstInterval, data.length - 1);
        const stepSegments = this.series.stepLocation === 0 || this.series.stepLocation === 1 ? 2 : 3;

        while (firstInterval < endInterval) {
            const physical = data.buffer.physicalIndexAt(firstInterval);
            const pageIndex = Math.floor(physical / ALIGNED_PAGE_SIZE);
            const pageOffset = physical % ALIGNED_PAGE_SIZE;
            const count = Math.min(endInterval - firstInterval, ALIGNED_PAGE_SIZE - pageOffset);
            this.gpu.drawPage(
                pageIndex, data.channel, pageOffset + DATA_OFFSET, count, this.series.lineType, stepSegments, program,
            );
            firstInterval += count;
        }
    }

    drawArea(renderMin: number, renderMax: number, program: AreaProgram) {
        const data = this.series.data;
        if (data.length < 2 || data.xAt(0) > renderMax || data.xAt(data.length - 1) < renderMin) return;

        let firstInterval = data.lowerBoundX(renderMin, 1, data.length) - 1;
        const endInterval = data.lowerBoundX(renderMax, firstInterval, data.length - 1);
        const stepSegments = this.series.stepLocation === 0 || this.series.stepLocation === 1 ? 2 : 3;
        while (firstInterval < endInterval) {
            const physical = data.buffer.physicalIndexAt(firstInterval);
            const pageIndex = Math.floor(physical / ALIGNED_PAGE_SIZE);
            const pageOffset = physical % ALIGNED_PAGE_SIZE;
            const count = Math.min(endInterval - firstInterval, ALIGNED_PAGE_SIZE - pageOffset);
            this.gpu.drawAreaPage(
                pageIndex, data.channel, pageOffset + DATA_OFFSET, count,
                this.series.lineType, stepSegments, program,
            );
            firstInterval += count;
        }
    }
}

export class LineChartRenderer {
    private lineProgram: LineProgram;
    private nativeLineProgram: NativeLineProgram;
    private areaProgram: AreaProgram;
    private uniformBuffer: ShaderUniformData;
    private buffers = new Map<AlignedDataBuffer, SharedGpuBuffer>();
    private seriesViews = new Map<TimeChartSeriesOptions, SeriesGpuView>();
    private height = 0;
    private width = 0;
    private renderHeight = 0;
    private renderWidth = 0;
    private xRangeStart = 0;
    private xRangeEnd = 0;
    private yRangeStart = 0;
    private yRangeEnd = 0;
    private xDomainMin = 0;
    private xDomainMax = 1;
    private yDomainMin = 0;
    private yDomainMax = 1;
    private xUnitsPerPixel = 1;
    private colorCache = new Map<TimeChartSeriesOptions, {
        source: ResolvedCoreOptions['color'] | TimeChartSeriesOptions['color'];
        rgba: ReturnType<typeof resolveColorRGBA>;
    }>();
    private fillColorCache = new Map<TimeChartSeriesOptions, {
        source: TimeChartSeriesOptions['fill'];
        rgba: ReturnType<typeof resolveColorRGBA>;
    }>();

    constructor(
        private model: RenderModel,
        private gl: WebGL2RenderingContext,
        private options: ResolvedCoreOptions,
    ) {
        // Native class fields run before constructor parameter properties are
        // assigned. Initialize these here so production ES2022 builds do not
        // read this.options while it is still undefined.
        this.lineProgram = new LineProgram(gl, options.debugWebGL);
        this.nativeLineProgram = new NativeLineProgram(gl, options.debugWebGL);
        this.areaProgram = new AreaProgram(gl, options.debugWebGL);
        const uboSize = gl.getActiveUniformBlockParameter(this.lineProgram.program, 0, gl.UNIFORM_BLOCK_DATA_SIZE);
        this.uniformBuffer = new ShaderUniformData(this.gl, uboSize);
        model.updated.on(() => this.drawFrame());
        model.resized.on((w, h) => this.onResize(w, h));
        model.disposing.on(() => this.dispose());
    }

    private viewFor(series: TimeChartSeriesOptions) {
        let gpu = this.buffers.get(series.data.buffer);
        if (!gpu) {
            gpu = new SharedGpuBuffer(this.gl, series.data.buffer);
            this.buffers.set(series.data.buffer, gpu);
        }
        let view = this.seriesViews.get(series);
        if (!view) {
            view = new SeriesGpuView(gpu, series);
            this.seriesViews.set(series, view);
        }
        return view;
    }

    syncBuffer() {
        for (const series of this.options.series) this.viewFor(series);
        for (const gpu of this.buffers.values()) gpu.sync();
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

    private fillColorFor(series: TimeChartSeriesOptions) {
        const source = series.fill!;
        const cached = this.fillColorCache.get(series);
        if (cached?.source === source) return cached.rgba;
        const rgba = resolveColorRGBA(source);
        this.fillColorCache.set(series, { source, rgba });
        return rgba;
    }

    drawFrame() {
        this.syncBuffer();
        this.syncDomain();
        const hasSeriesViewports = this.options.series.some(series => series.visible && series.viewport);
        if (!hasSeriesViewports) this.uniformBuffer.upload();
        const gl = this.gl;
        const renderMin = this.xDomainMin +
            (this.options.renderPaddingLeft - this.xRangeStart) * this.xUnitsPerPixel;
        const renderMax = this.xDomainMin +
            (this.width - this.options.renderPaddingRight - this.xRangeStart) * this.xUnitsPerPixel;

        // Draw fills first so every line remains crisp above its translucent
        // area. Both passes use the same resident paged textures.
        this.areaProgram.use();
        for (const series of this.options.series) {
            if (!series.visible || series.fill == null) continue;
            if (hasSeriesViewports) this.applySeriesViewport(series);
            gl.uniform4fv(this.areaProgram.locations.uColor, this.fillColorFor(series));
            gl.uniform1i(this.areaProgram.locations.uLineType, series.lineType);
            gl.uniform1f(this.areaProgram.locations.uStepLocation, series.stepLocation);
            gl.uniform1i(this.areaProgram.locations.uStepSegments,
                series.stepLocation === 0 || series.stepLocation === 1 ? 2 : 3);
            gl.uniform1f(this.areaProgram.locations.uBaseline, series.fillBaseline ?? 0);
            this.viewFor(series).drawArea(renderMin, renderMax, this.areaProgram);
        }

        let activeProgram: LineProgram | NativeLineProgram | null = null;
        for (const series of this.options.series) {
            if (!series.visible) continue;
            if (hasSeriesViewports) this.applySeriesViewport(series);
            const program = series.lineType === LineType.NativeLine || series.lineType === LineType.NativePoint
                ? this.nativeLineProgram : this.lineProgram;
            if (program !== activeProgram) {
                program.use();
                activeProgram = program;
            }
            gl.uniform4fv(program.locations.uColor, this.colorFor(series));

            const lineWidth = series.lineWidth ?? this.options.lineWidth;
            if (program instanceof LineProgram) {
                gl.uniform1i(program.locations.uLineType, series.lineType);
                gl.uniform1f(program.locations.uLineWidth, lineWidth / 2);
                if (series.lineType === LineType.Step) {
                    gl.uniform1f(program.locations.uStepLocation, series.stepLocation);
                    gl.uniform1i(program.locations.uStepSegments, series.stepLocation === 0 || series.stepLocation === 1 ? 2 : 3);
                }
            } else if (series.lineType === LineType.NativeLine) {
                gl.lineWidth(lineWidth * this.options.pixelRatio);
            } else {
                gl.uniform1f(program.locations.uPointSize, lineWidth * this.options.pixelRatio);
            }

            const linePad = lineWidth / 2 * this.xUnitsPerPixel;
            this.viewFor(series).draw(renderMin - linePad, renderMax + linePad, program);
        }
        if (this.options.debugWebGL) {
            const err = gl.getError();
            if (err !== gl.NO_ERROR) throw new Error(`WebGL error ${err}`);
        }
    }

    syncDomain() {
        const m = this.model;
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
        this.xDomainMax = xMax;
        this.yDomainMin = yMin;
        this.yDomainMax = yMax;
        this.xUnitsPerPixel = 1 / sx;
    }

    private applySeriesViewport(series: TimeChartSeriesOptions) {
        const viewport = series.viewport;
        if (!viewport) {
            const ratio = this.options.pixelRatio;
            this.gl.viewport(
                this.options.renderPaddingLeft * ratio,
                this.options.renderPaddingBottom * ratio,
                this.renderWidth * ratio,
                this.renderHeight * ratio,
            );
            this.syncDomain();
            this.uniformBuffer.upload();
            return;
        }

        const plotLeft = this.options.renderPaddingLeft;
        const plotTop = this.options.renderPaddingTop;
        const panelTop = plotTop + viewport.top * this.renderHeight;
        const panelBottom = plotTop + viewport.bottom * this.renderHeight - (viewport.gapAfter ?? 0);
        const panelHeight = panelBottom - panelTop;
        const ratio = this.options.pixelRatio;
        this.gl.viewport(
            plotLeft * ratio,
            (this.height - panelBottom) * ratio,
            this.renderWidth * ratio,
            panelHeight * ratio,
        );

        const xScale = this.renderWidth / (this.xDomainMax - this.xDomainMin);
        const yScale = panelHeight / (this.yDomainMax - this.yDomainMin);
        const uniforms = this.uniformBuffer;
        uniforms.projectionScale[0] = 2 / this.renderWidth;
        uniforms.projectionScale[1] = 2 / panelHeight;
        uniforms.modelScale[0] = xScale;
        uniforms.modelScale[1] = yScale;
        uniforms.modelTranslate[0] = -this.renderWidth / (2 * xScale) - this.xDomainMin;
        uniforms.modelTranslate[1] = -panelHeight / (2 * yScale) - this.yDomainMin;
        uniforms.upload();
    }

    private dispose() {
        for (const gpu of this.buffers.values()) gpu.delete();
        this.buffers.clear();
        this.seriesViews.clear();
        this.uniformBuffer.delete();
        this.gl.deleteProgram(this.lineProgram.program);
        this.gl.deleteProgram(this.nativeLineProgram.program);
        this.gl.deleteProgram(this.areaProgram.program);
    }
}

export const lineChart: TimeChartPlugin<LineChartRenderer> = {
    apply(chart) {
        return new LineChartRenderer(chart.model, chart.canvasLayer.gl, chart.options);
    },
};
