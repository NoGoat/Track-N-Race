import core from '../core';
import { TimeChartPlugins } from '../options';

export interface TimeChartPlugin<TState=unknown> {
    apply(chart: core<TimeChartPlugins>): TState;
}
