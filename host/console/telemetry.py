"""Validate the existing SGJSON v1 measurements before they affect UI state."""
import json
import re

U64=(1<<64)-1

def unsigned(value, maximum=U64):
    if not isinstance(value,str) or not re.fullmatch(r'0|[1-9][0-9]{0,19}',value):
        raise ValueError('noncanonical uint64')
    n=int(value)
    if n>maximum:raise ValueError('uint64 overflow')
    return n

def integer(data,key,low=0,high=(1<<32)-1):
    value=data[key]
    if type(value) is not int or not low<=value<=high:raise ValueError('invalid '+key)
    return value

def parse(line, channel):
    if not line.startswith('SGJSON ') or len(line)>4000:return None
    try:
        d=json.loads(line[7:])
        if not isinstance(d,dict) or type(d.get('schema')) is not int or d['schema']!=1:return None
        event=d['event']
        if event not in ('started','preflight','result','ended'):return None
        if event=='preflight':
            integer(d,'valid_mask',0,31);integer(d,'ddr');integer(d,'clocks');integer(d,'error',-4095,0)
            return d
        if d['source'] not in ('hardware','synthetic') or d['processor'] not in ('stats','pl'):return None
        if channel=='board' and d['source']!='hardware':return None
        if channel=='recorded-replay' and d['source']!='synthetic':return None
        if event=='started':
            integer(d,'queue_depth',1,16)
            if not 1<=unsigned(d['max_frame_bytes'])<=256*1024*1024:return None
            if d['continuity_verified'] is not False:return None
        elif event=='result':
            counts={k:unsigned(d[k]) for k in ('sequence','received_line_start','received_ns','processed_total','pixel_count','sum','above_threshold')}
            w=integer(d,'width',1);h=integer(d,'height',1);stride=integer(d,'stride',w)
            if stride*h>256*1024*1024 or w*h!=counts['pixel_count']:return None
            integer(d,'block_id');flags=integer(d,'flags',0,7);algorithm=integer(d,'algorithm')
            if bool(flags&2)!=(d['source']=='synthetic'):return None
            minimum=integer(d,'min',0,255);maximum=integer(d,'max',minimum,255)
            if not minimum*w*h<=counts['sum']<=maximum*w*h or counts['above_threshold']>w*h:return None
            if counts['sequence']<1 or not 1<=counts['processed_total']<=counts['sequence']:return None
            if d['decision'] not in ('unknown','pass','fail') or d['continuity_verified'] is not False:return None
            if (d['processor']=='stats' or algorithm==1) and d['decision']!='unknown':return None
        else:
            for key in ('error','source_error','processing_error','output_error','stop_error'):integer(d,key,-4095,0)
            if type(d['cancelled']) is not bool:return None
            counts={k:unsigned(d[k]) for k in ('received','processed','bytes','id_discontinuities','queue_full_events','elapsed_ms')}
            integer(d,'queue_peak',0,16)
            if counts['processed']>counts['received'] or counts['id_discontinuities']>counts['received']:return None
        return d
    except (ValueError,KeyError,TypeError,OverflowError):return None
