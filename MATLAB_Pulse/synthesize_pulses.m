function r = synthesize_pulses(c)
assert(ismember(c.mode,{'mixed','gamma','neutron','custom'}),'未知模式');
v=[c.fs c.points c.window c.gap c.rise c.peak c.noise c.baseline c.seed c.taper c.amplitude_jitter c.tau_jitter];
assert(all(isfinite(v)),'参数必须有限');
assert(c.fs>0 && c.points>=16 && c.points<=2e7 && mod(c.points,1)==0,'采样率或点数无效');
assert(min([c.window c.rise c.peak])>0 && min([c.gap c.noise c.taper])>=0,'时间、幅度或噪声无效');
assert(c.seed>=0 && c.seed<=2^32-1 && mod(c.seed,1)==0,'种子应为 uint32 范围整数');
assert(min([c.amplitude_jitter c.tau_jitter])>=0 && max([c.amplitude_jitter c.tau_jitter])<100,'波动范围须为 [0,100)');
for name={'gamma','neutron','custom'}
    ts=c.([name{1} '_taus']); ws=c.([name{1} '_weights']);
    assert(numel(ts)==3 && numel(ws)==3 && all(isfinite([ts ws])) && all(ts>0) && all(ws>=0) && sum(ws)>0,'三分量参数无效');
end
dt=1000/c.fs; support=floor(c.window/dt+.5); gap=floor(c.gap/dt+.5); slot=support+gap;
assert(support>=3 && support<=200000 && slot<=c.points && c.taper<(support-1)*dt,'窗口、收尾或间隔无效');
assert(c.preview_events>=1 && mod(c.preview_events,1)==0,'预览事件数须为正整数');
count=floor(c.points/slot);
previous=rng; restore=onCleanup(@() rng(previous)); %#ok<NASGU>
rng(c.seed,'twister');
% 先构造近1:1标签，再随机置换；奇数时 gamma 多一个。
if strcmp(c.mode,'mixed'), kinds=mod((0:count-1)',2); kinds=kinds(randperm(count));
else, kinds=repmat(double(strcmp(c.mode,'neutron')),count,1); end
clean=repmat(c.baseline,c.points,1); representatives=struct;
labels=cell(count,1); amplitudes=zeros(count,1); taus=zeros(count,3); rises=zeros(count,1);
for i=1:count
    if strcmp(c.mode,'mixed')
        if kinds(i)==0, name='gamma'; else, name='neutron'; end
    else, name=c.mode; end
    ts=c.([name '_taus']); ws=c.([name '_weights']);
    amplitudes(i)=c.peak*(1+(2*rand-1)*c.amplitude_jitter/100);
    taus(i,:)=ts.*(1+(2*rand(1,3)-1)*c.tau_jitter/100);
    rises(i)=c.rise*(1+(2*rand-1)*c.tau_jitter/100);
    parts=pulse_parts(c,taus(i,:),ws,rises(i),amplitudes(i),support,dt);
    idx=(i-1)*slot+(1:support); clean(idx)=clean(idx)+sum(parts,2);
    if ~isfield(representatives,name), representatives.(name)=parts; end
    labels{i}=name;
end
noise=randn(c.points,1); noise=noise-mean(noise); noise=noise*(c.noise/sqrt(mean(noise.^2)));
noisy=clean+noise; divisor=1;
if c.normalize, divisor=max([1;abs(noisy);abs(clean)]); end
r.clean=clean/divisor; r.noisy=noisy/divisor; r.noise=noise/divisor;
for name=fieldnames(representatives)'
    representatives.(name{1})=representatives.(name{1})/divisor;
end
r.representatives=representatives; r.kinds=kinds; r.dt=dt; r.support=support;
r.gap=gap; r.slot=slot; r.divisor=divisor;
event_id=(1:count)'; start_sample=(event_id-1)*slot+1; start_ns=(start_sample-1)*dt;
r.events=table(event_id,labels,start_sample,start_ns,amplitudes,taus(:,1),taus(:,2),taus(:,3),rises, ...
    'VariableNames',{'event_id','particle','start_sample','start_ns','peak_before_scale','tau_fast_ns','tau_mid_ns','tau_slow_ns','rise_ns'});
end
