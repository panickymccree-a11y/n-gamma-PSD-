function export_awg(path,c,r,which)
header={sprintf('data length,%d',c.points),sprintf('frequency,%.12g',c.fs*1e6/c.points), ...
    'amp,2.000000','offset,0.000000','phase,0.000000','','','','','','','','xpos,value'};
tmp=[path '.tmp']; fid=fopen(tmp,'w'); assert(fid>=0,'无法创建 CSV');
try
    for j=1:13, fprintf(fid,'%s\r\n',header{j}); end
    signal=r.(which);
    for first=1:50000:c.points
        last=min(c.points,first+49999);
        fprintf(fid,'%d,%.9f\r\n',[(first:last);signal(first:last)']);
    end
    fclose(fid); fid=-1;
    fid=fopen(tmp,'r');
    for j=1:13, assert(strcmp(fgetl(fid),header{j}),'CSV头部不匹配'); end
    count=0;
    while ~feof(fid)
        block=textscan(fid,'%f%f',50000,'Delimiter',',');
        if isempty(block{1}), break; end
        m=numel(block{1});
        assert(numel(block{2})==m && all(block{1}==(count+1:count+m)') && all(isfinite(block{2})),'CSV数据校验失败');
        count=count+m;
    end
    assert(count==c.points,'CSV点数不匹配'); fclose(fid); fid=-1;
    movefile(tmp,path,'f');
catch err
    if fid>=0, fclose(fid); end
    if exist(tmp,'file'), delete(tmp); end
    rethrow(err);
end
meta=struct('config',c,'signal',which,'divisor',r.divisor,'actual_window_ns',r.support*r.dt, ...
    'actual_gap_ns',r.gap*r.dt,'event_count',height(r.events),'export_noise_rms',std(r.noise,1)*strcmp(which,'noisy'), ...
    'record_frequency_Hz',c.fs*1e6/c.points,'provenance','Synthetic model; no hardware validation');
fid=fopen([path '.json'],'w','n','UTF-8'); assert(fid>=0,'无法创建元数据');
closer=onCleanup(@() fclose(fid)); %#ok<NASGU>
fprintf(fid,'%s',jsonencode(meta));
end
