import { memo, useState } from 'react'
import type { StrategyPlan, StrategySnapshotMsg, StrategyStint } from '../types'
import type { DensityMode } from '../lib/graphSections'

interface Props { strategy: StrategySnapshotMsg | null; isDark: boolean; compact?: DensityMode | boolean }

const COLORS: Record<number, string> = { 16:'var(--compound-soft)',17:'var(--compound-medium)',18:'var(--compound-hard)',7:'var(--compound-inter)',8:'var(--compound-wet)' }
const wearColor=(v:number)=>v<20?'#73BF69':v<40?'#A8D436':v<60?'#FADE2A':v<80?'#FF9830':'#C4162A'
const lapTime=(ms:number)=>{if(ms<=0||ms>600000)return '—';const s=ms/1000;return `${Math.floor(s/60)}:${(s%60).toFixed(1).padStart(4,'0')}`}
const delta=(ms:number)=>`${ms>0?'+':ms<0?'−':''}${Math.abs(ms/1000).toFixed(1)}`
const words=(value:string)=>value.replace(/_/g,' ')

const Chip=memo(({name,visual}:{name:string;visual:number})=>{const c=COLORS[visual]??'var(--text-primary)';return <span className="text-[10px] font-bold px-2 py-0.5 rounded border shrink-0" style={{color:c,borderColor:c,backgroundColor:`color-mix(in srgb, ${c} 10%, transparent)`}}>{name}</span>})

const Stint=memo(({stint,separator}:{stint:StrategyStint;separator:boolean})=><div className={separator?'border-t border-[var(--border)]':''}>
  <div className="flex items-center gap-2 px-4 py-2"><Chip name={stint.compound_name} visual={stint.visual_compound}/><b className="text-sm">{stint.stint_number}</b><span className="flex-1"/><span className="text-[10px] text-[var(--text-secondary)]">{stint.start_lap}–{stint.end_lap}</span><span className="flex-1"/><span className="text-[9px] uppercase text-[var(--text-secondary)]">Expected</span><b>{stint.expected_laps}</b><span className="text-[9px] uppercase text-[var(--text-secondary)]">Actual</span><b>{stint.actual_laps}</b></div>
  {stint.rows.length>0&&<table className="w-full border-collapse"><thead><tr className="border-y border-[var(--border)]">{['LAP','REQ','ACTUAL','Δ LAP','Δ STINT','Δ TOTAL'].map(h=><th key={h} className="px-2 py-1 text-[9px] font-normal text-[var(--text-secondary)]">{h}</th>)}</tr></thead><tbody>{stint.rows.map((r,i)=><tr key={r.lap_num} className={!stint.is_last&&r.lap_num===stint.end_lap?'bg-[#73BF69]/15':i%2?'bg-[var(--bg-input)]/30':''}>
    <td className="px-2 py-1 text-center text-[11px]">{r.lap_num}</td><td className="px-2 py-1 text-center text-[11px]">{lapTime(r.required_ms)}</td><td className="px-2 py-1 text-center text-[11px]">{r.has_actual?lapTime(r.actual_ms):'—'}</td>
    {[r.delta_lap_ms,r.delta_stint_ms,r.delta_total_ms].map((v,j)=><td key={j} className="px-2 py-1 text-center text-[11px]" style={{color:r.has_actual?(v>0?'#C4162A':'#73BF69'):'var(--text-muted)'}}>{r.has_actual?delta(v):'—'}</td>)}
  </tr>)}</tbody></table>}
</div>)

const PlanColumn=memo(({plan,label,accent}:{plan:StrategyPlan;label:string;accent:string})=><div className="flex flex-col h-full min-h-0"><div className="shrink-0 px-4 py-2.5 border-b border-[var(--border)]"><div className="flex items-center"><span className="text-[10px] uppercase tracking-widest text-[var(--text-secondary)]">{label}</span>{!plan.legal&&<span className="ml-2 text-[9px] font-bold px-1.5 py-0.5 rounded border text-[#C4162A] border-[#C4162A]">No legal set path</span>}<span className="flex-1"/><b className="text-2xl" style={{color:accent}}>{plan.stops}</b><span className="ml-1 text-xs text-[var(--text-secondary)]">stop{plan.stops===1?'':'s'}</span></div><div className="mt-1 text-[10px] text-[var(--text-secondary)] truncate">{plan.target_idx>=0?`${plan.mode==='attacking'?'Chasing':'Covering'} ${plan.target_name}`:'Tyre-life baseline'} · {Math.round(plan.confidence*100)}% confidence{plan.requires_compound_change?' · compound change required':''}</div></div><div className="flex-1 overflow-y-auto">{plan.stints.map((s,i)=><Stint key={`${s.stint_number}-${s.start_lap}`} stint={s} separator={i>0}/>)}</div></div>)

function Header({s,compact}:{s:StrategySnapshotMsg|null;compact?:DensityMode|boolean}) {
  const ready = s?.state === 'ready'
  const wear = ready ? Math.round(s.average_wear) : 0
  const wearBar = wearColor(wear)
  const cliffColor = ready
    ? s.laps_until_cliff <= 5 ? '#C4162A' : s.laps_until_cliff <= 10 ? '#FADE2A' : 'var(--text-primary)'
    : 'var(--text-secondary)'

  const isCompact = compact === true || compact === 'compact'
  const isSpacious = compact === 'spacious'

  if (isCompact) return <div className="shrink-0 flex divide-x divide-[var(--border)] border-b border-[var(--border)]">
    <div className="shrink-0 flex items-baseline gap-1.5 px-6 py-1.5">
      <span className="text-lg font-black tabular-nums leading-none text-[var(--text-primary)]">{s?.lap_num || '—'}</span>
      <span className="text-xs font-medium text-[var(--text-secondary)]">/ {s?.total_laps || '—'}</span>
    </div>
    <div className="flex-1 min-w-0 flex items-center gap-3 px-6 py-1.5">
      <Chip name={ready?s.current_compound_name:'—'} visual={s?.current_visual_compound??0}/>
      <span className="text-sm font-black tabular-nums leading-none shrink-0" style={{color:ready?wearBar:'var(--text-secondary)'}}>{ready?`${wear}%`:'—'}</span>
      <div className="flex-1 min-w-0 h-1.5 bg-[var(--border)] rounded-full overflow-hidden">
        <div className="h-full rounded-full transition-all duration-300" style={{width:`${ready?Math.min(100,wear):0}%`,backgroundColor:wearBar}}/>
      </div>
      <span className="text-[10px] text-[var(--text-secondary)] tabular-nums shrink-0">{ready?`${s.current_tyre_age_laps}L · ${s.wear_per_lap.toFixed(1)}%/L`:'—'}</span>
    </div>
    <div className="shrink-0 flex items-center gap-2 px-6 py-1.5">
      <span className="text-[10px] font-medium uppercase tracking-widest text-[var(--text-secondary)]">Tyre Cliff</span>
      <span className="text-sm font-black tabular-nums leading-none" style={{color:cliffColor}}>{ready?`Lap ${s.cliff_lap}`:'—'}</span>
      {ready&&<span className="text-[10px] text-[var(--text-secondary)]">+{s.laps_until_cliff}</span>}
    </div>
  </div>

  if (isSpacious) return <div className="shrink-0 flex divide-x divide-[var(--border)] border-b border-[var(--border)]">
    <div className="shrink-0 flex flex-col justify-center px-8 py-5">
      <span className="text-xs font-bold uppercase tracking-widest text-[var(--text-secondary)] mb-1.5">Lap</span>
      <div className="flex items-baseline gap-2">
        <span className="text-4xl font-black tabular-nums leading-none">{s?.lap_num || '—'}</span>
        <span className="text-lg font-semibold text-[var(--text-secondary)]">/ {s?.total_laps || '—'}</span>
      </div>
      {s?.total_laps && s.total_laps > 0 && s.lap_num > 0 && (
        <span className="text-[11px] font-medium text-[var(--text-secondary)] mt-1 tabular-nums">
          {Math.round((s.lap_num / s.total_laps) * 100)}% distance
        </span>
      )}
    </div>
    <div className="flex-1 min-w-0 flex items-center gap-6 px-8 py-5">
      <span className="text-xs font-black px-3 py-1 rounded border shrink-0" style={{color:COLORS[s?.current_visual_compound??0]??'var(--text-primary)',borderColor:COLORS[s?.current_visual_compound??0]??'var(--text-primary)',backgroundColor:`color-mix(in srgb, ${COLORS[s?.current_visual_compound??0]??'var(--text-primary)'} 10%, transparent)`}}>
        {ready ? s.current_compound_name : '—'}
      </span>
      <div className="flex-1 min-w-0">
        <div className="flex items-baseline justify-between gap-4 mb-2">
          <span className="text-2xl font-black tabular-nums leading-none" style={{color:ready?wearBar:'var(--text-secondary)'}}>{ready?`${wear}%`:'—'}</span>
          <span className="text-xs font-bold text-[var(--text-secondary)] tabular-nums shrink-0">{ready?`${s.current_tyre_age_laps}L age · ${s.wear_per_lap.toFixed(1)}%/L`:'—'}</span>
        </div>
        <div className="h-2.5 bg-[var(--border)] rounded-full overflow-hidden">
          <div className="h-full rounded-full transition-all duration-300" style={{width:`${ready?Math.min(100,wear):0}%`,backgroundColor:wearBar}}/>
        </div>
        {ready && s.limiting_corner && (
          <div className="text-[11px] font-medium text-[var(--text-secondary)] mt-1.5 truncate">
            {s.limiting_corner} is limiting tyre
          </div>
        )}
      </div>
    </div>
    <div className="shrink-0 flex flex-col justify-center px-8 py-5">
      <span className="text-xs font-bold uppercase tracking-widest text-[var(--text-secondary)] mb-1.5">Tyre Cliff</span>
      <div className="flex items-baseline gap-2">
        <span className="text-2xl font-black tabular-nums leading-none" style={{color:cliffColor}}>{ready?`Lap ${s.cliff_lap}`:'—'}</span>
        {ready&&<span className="text-xs font-bold text-[var(--text-secondary)]">+{s.laps_until_cliff}L</span>}
      </div>
      {ready && (
        <span className="text-[11px] font-medium text-[var(--text-secondary)] mt-1 tabular-nums">
          {s.laps_until_cliff} laps remaining
        </span>
      )}
    </div>
  </div>

  return <div className="shrink-0 flex divide-x divide-[var(--border)] border-b border-[var(--border)]">
    <div className="shrink-0 flex flex-col justify-center px-6 py-3">
      <span className="text-[10px] font-medium uppercase tracking-widest text-[var(--text-secondary)] mb-1">Lap</span>
      <div className="flex items-baseline gap-1.5"><span className="text-3xl font-black tabular-nums leading-none">{s?.lap_num||'—'}</span><span className="text-base font-medium text-[var(--text-secondary)]">/ {s?.total_laps||'—'}</span></div>
    </div>
    <div className="flex-1 min-w-0 flex items-center gap-4 px-6 py-3">
      <Chip name={ready?s.current_compound_name:'—'} visual={s?.current_visual_compound??0}/>
      <div className="flex-1 min-w-0">
        <div className="flex items-baseline justify-between gap-3 mb-1.5"><span className="text-lg font-black tabular-nums leading-none" style={{color:ready?wearBar:'var(--text-secondary)'}}>{ready?`${wear}%`:'—'}</span><span className="text-[10px] text-[var(--text-secondary)] tabular-nums shrink-0">{ready?`${s.current_tyre_age_laps}L · ${s.wear_per_lap.toFixed(1)}%/L`:'—'}</span></div>
        <div className="h-1.5 bg-[var(--border)] rounded-full overflow-hidden"><div className="h-full rounded-full transition-all duration-300" style={{width:`${ready?Math.min(100,wear):0}%`,backgroundColor:wearBar}}/></div>
      </div>
    </div>
    <div className="shrink-0 flex flex-col justify-center px-6 py-3">
      <span className="text-[10px] font-medium uppercase tracking-widest text-[var(--text-secondary)] mb-1">Tyre Cliff</span>
      <div className="flex items-baseline gap-1.5"><span className="text-lg font-black tabular-nums leading-none" style={{color:cliffColor}}>{ready?`Lap ${s.cliff_lap}`:'—'}</span>{ready&&<span className="text-[10px] text-[var(--text-secondary)]">+{s.laps_until_cliff}</span>}</div>
    </div>
  </div>
}

const nextStopLap=(plan:StrategyPlan,currentLap:number)=>plan.stints.find(stint=>!stint.is_last&&stint.end_lap>=currentLap)?.end_lap
const stopText=(lap:number|undefined,currentLap:number)=>lap===undefined?'FLAG':lap<=currentLap?'NOW':`L${lap}`

const WARNING_STYLES = {
  danger: '#C4162A',
  warning: '#FF9830',
  caution: '#FADE2A',
} as const

const shortTyreScope=(value:string)=>value
  .replace(/front-left/gi,'FL').replace(/front-right/gi,'FR')
  .replace(/rear-left/gi,'RL').replace(/rear-right/gi,'RR')
  .replace(/\s+tyres?$/i,'').replace(/\s+and\s+/i,' + ')
  .toUpperCase()

const tyreScopeLabel=(value:string)=>{
  const scope=shortTyreScope(value)
  return `${scope} ${/tyres/i.test(value)||scope.includes('+')?'TYRES':'TYRE'}`
}

function wearWarningParts(text:string) {
  let match=text.match(/^(.+?) is the limiting tyre$/i)
  if(match)return {scope:tyreScopeLabel(match[1]),detail:'is limiting'}
  match=text.match(/^(.+?) wear far above average$/i)
  if(match)return {scope:tyreScopeLabel(match[1]),detail:'wear far above average'}
  match=text.match(/^(.+?) wearing faster than (.+)$/i)
  if(match)return {scope:tyreScopeLabel(match[1]),detail:`High wear rate vs ${shortTyreScope(match[2])}`}
  match=text.match(/^(.+?) wearing faster$/i)
  if(match)return {scope:tyreScopeLabel(match[1]),detail:'High wear rate'}
  return {scope:'',detail:text}
}

function WearWarnings({warnings}:{warnings:StrategySnapshotMsg['wear_warnings']}) {
  if (warnings.length===0) return null
  return <section>
    <div className="px-4 pt-3 pb-2 text-[10px] uppercase tracking-widest text-[var(--text-secondary)]">Tyre alerts</div>
    <div>
      {warnings.map(w=>{const color=WARNING_STYLES[w.severity],parts=wearWarningParts(w.text);return <div key={w.text} className="flex min-w-0 items-center gap-2 border-t border-[var(--border)] px-4 py-2">
        <span className="shrink-0 text-xs leading-none" style={{color}} aria-hidden="true">⚠</span>
        <div className="min-w-0 flex items-baseline gap-1.5">
          {parts.scope&&<b className="shrink-0 text-[11px] font-bold" style={{color}}>{parts.scope}</b>}
          <span className="truncate text-[10px] text-[var(--text-secondary)]">{parts.detail}</span>
        </div>
      </div>})}
    </div>
  </section>
}

interface MinimumStopsControlProps { value:number; onChange:(value:number)=>void }

const clampMinimumStops=(value:unknown)=>Math.min(8,Math.max(0,Math.trunc(Number(value)||0)))

function MinimumStopsControl({value,onChange}:MinimumStopsControlProps) {
  return <div className="shrink-0 border-t border-[var(--border)] px-5 py-2.5">
    <div className="flex items-center gap-3">
      <label htmlFor="strategy-minimum-stops" className="min-w-0 flex-1 text-[9px] uppercase tracking-[0.16em] text-[var(--text-secondary)]">Required pit stops</label>
      <div className="flex h-7 w-16 overflow-hidden rounded-md border border-[var(--border)] bg-[var(--bg-input)]">
        <input id="strategy-minimum-stops" type="number" min={0} max={8} step={1} value={value} onChange={event=>onChange(clampMinimumStops(event.target.value))} onWheel={event=>event.currentTarget.blur()} className="no-number-spinner min-w-0 flex-1 bg-transparent px-0 text-center text-xs font-normal tabular-nums text-[var(--text-primary)] outline-none" aria-label="Required pit stops"/>
        <div className="flex w-3.5 shrink-0 flex-col border-l border-[var(--border)]">
          <button type="button" onClick={()=>onChange(Math.min(8,value+1))} disabled={value>=8} className="grid flex-1 place-items-center text-[6px] leading-none text-[var(--text-secondary)] outline-none hover:bg-[var(--bg-hover)] hover:text-[var(--text-primary)] disabled:opacity-25" aria-label="Increase required pit stops">▲</button>
          <button type="button" onClick={()=>onChange(Math.max(0,value-1))} disabled={value<=0} className="grid flex-1 place-items-center border-t border-[var(--border)] text-[6px] leading-none text-[var(--text-secondary)] outline-none hover:bg-[var(--bg-hover)] hover:text-[var(--text-primary)] disabled:opacity-25" aria-label="Decrease required pit stops">▼</button>
        </div>
      </div>
    </div>
  </div>
}

function WaitingSidebar({blue,amber,minimumStops,onMinimumStopsChange}:{blue:string;amber:string;minimumStops:number;onMinimumStopsChange:(value:number)=>void}) {
  return <div className="w-80 shrink-0 min-h-0 flex flex-col">
    <div className="flex-1 min-h-0 overflow-y-auto divide-y divide-[var(--border)]">
    <section>
      <div className="px-4 pt-3 pb-2 text-[10px] uppercase tracking-widest text-[var(--text-secondary)]">Next pit window</div>
      {([{label:'Defend',color:blue},{label:'Attack',color:amber}] as const).map(option=><div key={option.label} className="flex items-center gap-2 border-t border-[var(--border)] px-4 py-2.5">
        <div className="min-w-0 flex-1"><b className="block text-[9px] uppercase tracking-widest" style={{color:option.color}}>{option.label}</b><span className="mt-0.5 block truncate text-[10px] text-[var(--text-secondary)]">-</span></div>
        <b className="shrink-0 text-lg font-black tabular-nums text-[var(--text-secondary)]">-</b>
      </div>)}
    </section>
    <section className="p-4">
      <div className="text-[10px] uppercase tracking-widest text-[var(--text-secondary)] mb-2">Weather window</div>
      <div className="flex items-baseline gap-2"><b className="text-sm text-[var(--text-secondary)]">-</b><b className="ml-auto text-xs text-[var(--text-secondary)]">-</b></div>
      <p className="text-[10px] text-[var(--text-secondary)] mt-1">-</p>
    </section>
    <section>
      <div className="px-4 pt-3 pb-1 flex items-baseline"><h3 className="text-[10px] uppercase tracking-widest text-[var(--text-secondary)]">Tyre condition</h3><span className="ml-auto text-[10px] text-[var(--text-secondary)]">-</span></div>
      <div className="grid grid-cols-2">{(['FL','FR','RL','RR'] as const).map(n=><div key={n} className="px-4 py-2"><div className="flex"><span className="text-[10px] text-[var(--text-secondary)]">{n}</span><b className="ml-auto text-[var(--text-secondary)]">-</b></div><div className="h-1 mt-1 rounded bg-[var(--border)]"/></div>)}</div>
    </section>
    </div>
    <MinimumStopsControl value={minimumStops} onChange={onMinimumStopsChange}/>
  </div>
}

function Sidebar({s,blue,amber,minimumStops,onMinimumStopsChange}:{s:StrategySnapshotMsg;blue:string;amber:string;minimumStops:number;onMinimumStopsChange:(value:number)=>void}) {
  const defensiveStop=nextStopLap(s.conservative,s.lap_num)
  const attackingStop=nextStopLap(s.aggressive,s.lap_num)
  const weather=s.weather_strategy&&s.weather_strategy.crossover_lap>0?s.weather_strategy:null
  return <div className="w-80 shrink-0 min-h-0 flex flex-col">
    <div className="flex-1 min-h-0 overflow-y-auto divide-y divide-[var(--border)]">
    {s.neutralisation&&<section className="p-4" style={{background:s.neutralisation.recommendation==='box'?'color-mix(in srgb, #73BF69 12%, transparent)':'color-mix(in srgb, #FADE2A 10%, transparent)'}}>
      <div className="text-[10px] uppercase tracking-widest text-[var(--text-secondary)]">{s.neutralisation.kind==='safety_car'?'Safety Car decision':'VSC decision'}</div>
      <div className="mt-2 flex items-end gap-2"><b className="text-xl">{s.neutralisation.recommendation==='box'?'BOX NOW':'STAY OUT'}</b><span className="ml-auto text-xs">P{s.neutralisation.current_position} → P{s.neutralisation.recommendation==='box'?s.neutralisation.projected_box_position:s.neutralisation.projected_stay_position}</span></div>
      <p className="mt-1 text-[10px] capitalize text-[var(--text-secondary)]">{words(s.neutralisation.reason)}</p>
      <div className="mt-3 grid grid-cols-2 gap-2 text-[10px]"><div><span className="block text-[var(--text-secondary)]">Box now</span><b>{delta(s.neutralisation.box_now_cost_ms)}s</b></div><div><span className="block text-[var(--text-secondary)]">Wait to L{s.neutralisation.box_later_lap}</span><b>{delta(s.neutralisation.box_later_cost_ms)}s</b></div></div>
    </section>}

    {s.call&&<section className="p-4">
      <div className="text-[10px] uppercase tracking-widest text-[var(--text-secondary)] mb-2">Race call</div>
      <div className="flex gap-2 items-center"><span className={`text-[9px] font-bold border rounded px-1.5 py-0.5 ${s.call.kind==='undercut'||s.call.kind==='cover'?'text-[#FADE2A] border-[#FADE2A]':'text-[#73BF69] border-[#73BF69]'}`}>{s.call.kind.toUpperCase()}</span><b className="truncate flex-1">{s.call.target_name}</b><b className="text-xs">{(s.call.gap_ms/1000).toFixed(1)}s</b></div>
      <p className="text-[10px] text-[var(--text-secondary)] mt-2 capitalize">{words(s.call.reason)}{s.call.crossover_laps!==undefined?` · ${s.call.crossover_laps} lap${s.call.crossover_laps===1?'':'s'}`:''}</p>
    </section>}

    <section>
      <div className="px-4 pt-3 pb-2 text-[10px] uppercase tracking-widest text-[var(--text-secondary)]">Next pit window</div>
      {([
        {label:'Defend',stop:defensiveStop,target:s.conservative.target_idx>=0?`Cover ${s.conservative.target_name}`:'Tyre life',color:blue},
        {label:'Attack',stop:attackingStop,target:s.aggressive.target_idx>=0?`Chase ${s.aggressive.target_name}`:'Tyre life',color:amber},
      ] as const).map(option=><div key={option.label} className="flex items-center gap-2 border-t border-[var(--border)] px-4 py-2.5">
        <div className="min-w-0 flex-1"><b className="block text-[9px] uppercase tracking-widest" style={{color:option.color}}>{option.label}</b><span className="mt-0.5 block truncate text-[10px] text-[var(--text-secondary)]">{option.target}</span></div>
        <b className="shrink-0 text-lg font-black tabular-nums" style={{color:option.color}}>{stopText(option.stop,s.lap_num)}</b>
      </div>)}
    </section>

    {weather&&<section className="p-4">
      <div className="text-[10px] uppercase tracking-widest text-[var(--text-secondary)] mb-2">Weather window</div>
      <div className="flex items-baseline gap-2"><b className="text-sm capitalize">{words(weather.recommendation)}</b><b className="ml-auto text-xs">L{weather.crossover_lap}</b></div>
      <p className="text-[10px] text-[var(--text-secondary)] mt-1">{weather.rain_percentage}% rain · about {weather.minutes_until_change} min</p>
    </section>}

    {s.rivals.length>0&&<section>
      <h3 className="px-4 pt-3 pb-2 text-[10px] uppercase tracking-widest text-[var(--text-secondary)]">Race battle</h3>
      {s.rivals.map(r=>{const pace=Math.abs(r.pace_delta_ms)>50?`${Math.abs(r.pace_delta_ms/1000).toFixed(2)}s/L ${r.pace_delta_ms<0?'faster':'slower'}`:'Matched pace';const stopped=r.last_pit_lap>=s.lap_num-1;return <div key={r.idx} className="px-4 py-2 border-t border-[var(--border)]">
        <div className="flex items-center gap-2"><span className="text-[10px] font-bold text-[var(--text-secondary)]">P{r.position}</span><b className="min-w-0 flex-1 truncate text-sm">{r.name}</b><span className="text-xs tabular-nums">{r.direction==='ahead'?'−':'+'}{(r.gap_ms/1000).toFixed(1)}s</span></div>
        <div className="mt-1 flex text-[9px] text-[var(--text-secondary)]"><span>{r.direction==='ahead'?'To catch':'To cover'} · {r.tyre_age_laps}L tyres</span><span className="ml-auto">{stopped?`stopped L${r.last_pit_lap}`:pace}</span></div>
      </div>})}
    </section>}

    <section>
      <div className="px-4 pt-3 pb-1 flex items-baseline"><h3 className="text-[10px] uppercase tracking-widest text-[var(--text-secondary)]">Tyre condition</h3><span className="ml-auto text-[10px] text-[var(--text-secondary)]">{s.limiting_corner} limits · cliff L{s.cliff_lap}</span></div>
      <div className="grid grid-cols-2">{([['FL',s.wear_fl],['FR',s.wear_fr],['RL',s.wear_rl],['RR',s.wear_rr]] as const).map(([n,v])=><div key={n} className="px-4 py-2"><div className="flex"><span className="text-[10px] text-[var(--text-secondary)]">{n}</span><b className="ml-auto" style={{color:wearColor(v)}}>{Math.round(v)}%</b></div><div className="h-1 mt-1 rounded bg-[var(--border)]"><div className="h-full rounded" style={{width:`${Math.min(100,v)}%`,background:wearColor(v)}}/></div></div>)}</div>
    </section>
    <WearWarnings warnings={s.wear_warnings}/>
    </div>
    <MinimumStopsControl value={minimumStops} onChange={onMinimumStopsChange}/>
  </div>
}

const StrategyPanel=memo(function StrategyPanel({strategy,isDark,compact}:Props){
  const blue=isDark?'#5794F2':'#0B57D0'
  const amber=isDark?'#FADE2A':'#8B5200'
  const [minimumStops,setMinimumStops]=useState(1)
  const changeMinimumStops=(value:number)=>{
    const stops=clampMinimumStops(value)
    setMinimumStops(stops)
    window.strategyBridge.setMinimumStops(stops)
  }

  return <div className="flex flex-col h-full overflow-hidden">
    <Header s={strategy} compact={compact}/>
    {strategy?.state==='non_race'
      ? <div className="flex-1 flex flex-col items-center justify-center"><b>Race sessions only</b><span className="text-xs text-[var(--text-secondary)] mt-2">Strategy suggestions are available during Race, Race 2, and Race 3 sessions.</span></div>
      : strategy?.state!=='ready'
        ? <div className="flex flex-1 min-h-0 divide-x divide-[var(--border)]"><div className="flex-1 flex items-center justify-center text-[var(--text-secondary)]">Waiting for tyre data…</div><WaitingSidebar blue={blue} amber={amber} minimumStops={minimumStops} onMinimumStopsChange={changeMinimumStops}/></div>
        : <div className="flex flex-1 min-h-0 divide-x divide-[var(--border)]"><div className="flex-1 min-w-0"><PlanColumn plan={strategy.conservative} label="Defensive" accent={blue}/></div><div className="flex-1 min-w-0"><PlanColumn plan={strategy.aggressive} label="Attacking" accent={amber}/></div><Sidebar s={strategy} blue={blue} amber={amber} minimumStops={minimumStops} onMinimumStopsChange={changeMinimumStops}/></div>}
  </div>
})
export default StrategyPanel
