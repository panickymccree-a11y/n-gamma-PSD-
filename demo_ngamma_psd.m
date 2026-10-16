%% 参数化塑闪 n/gamma PSD 演示（不依赖额外工具箱）
% 运行：demo_ngamma_psd
% 本模型用于算法/电子学原理演示；参数未经真实探测器标定。
% 模型结果不能作为 250 keVee 下实际 FOM 指标的证明。
clear; close all; clc;
rng(42);
outDir = fullfile(fileparts(mfilename('fullpath')), 'demo_output');
if ~exist(outDir,'dir'), mkdir(outDir); end

%% 可调参数：时间 ns，能量 keVee，电压 mV
p.fsMHz = 250;
p.t = (-160:1000/p.fsMHz:1600)';
p.N = 6000;                    % 每类事件数
p.energyRange = [100 600];
p.material = 'EJ-276D';
p.sourceURL = 'https://eljentechnology.com/index.php/products/loadedscintillators/79-ej-256';
p.tauGamma = [13 35 270];      % 厂家网页：前三个近似平均衰减时间 ns
p.tauNeutron = [13 59 460];    % 2025 目录第二项为 50 ns，网页为 59 ns
p.lightYield = 8600;           % 厂家：photons/MeV electron，仅材料元数据
p.emissionPeak = 425;          % 厂家：nm；不用于推算传感器效率
p.density = 1.099;             % 厂家：g/cm^3
p.tauRise = 8;                % 假设：有效上升常数，非厂家测量值
p.weightsGamma = [0.80 0.15 0.05]; % 假设：积分光量占比，不是幅度占比
p.weightsNeutron = [0.65 0.23 0.12];
p.weightSigma = 0.012;         % 假设：事件间权重扰动，截断后归一化
p.gain = 20;                    % 总面积/能量，mV*ns/keVee
p.noiseRMS = 0.5;             % ADC 输入噪声 mV
p.tauElectronics = 5;          % 一阶低通时间常数；0 为旁路
p.adcBits = 14;
p.adcFullScale = 1000;         % 双极性总量程 mV
p.triggerJitter = 4;           % 触发对齐残差，标准差 ns
p.gateTotal = [0 1200];        % 长尾窗口；尚未针对 FOM 优化
p.gateTail = [100 1200];
p.fomWindow = [225 275];       % 使用真值能量选择窗口
p.fallLevels = [0.8 0.2];      % 峰后 80% -> 20% 的下降时间

%% 生成独立事件并逐事件计算 PSD
energy = p.energyRange(1) + diff(p.energyRange)*rand(2*p.N,1);
isNeutron = [false(p.N,1); true(p.N,1)];
weights = repmat(p.weightsGamma,2*p.N,1);
weights(isNeutron,:) = repmat(p.weightsNeutron,p.N,1);
weights = max(weights+p.weightSigma*randn(size(weights)),0.001);
weights = weights./sum(weights,2);
dt = 1000/p.fsMHz;
qTotal = zeros(size(energy)); qTail = qTotal;
meanTime = nan(size(energy)); fallTime = meanTime;
examples = zeros(numel(p.t),2);
for k = 1:numel(energy)
    w = makePulse(p.t,energy(k),isNeutron(k),weights(k,:),p.triggerJitter*randn,p);
    % 预触发均值估计基线，积分窗口相对名义触发时刻
    w = w - mean(w(p.t < -40));
    qTotal(k) = sum(w(p.t>=p.gateTotal(1) & p.t<p.gateTotal(2)))*dt;
    qTail(k) = sum(w(p.t>=p.gateTail(1) & p.t<p.gateTail(2)))*dt;
    gate = p.t>=p.gateTotal(1) & p.t<p.gateTotal(2);
    % 保留带符号的基线扣除样本，避免截断噪声造成均值时间偏置
    meanTime(k) = sum(p.t(gate).*w(gate))/sum(w(gate));
    fallTime(k) = pulseFallTime(p.t,w,p);
end
psd = qTail./qTotal;
energyMeasured = qTotal/p.gain;
valid = isfinite(psd) & qTotal>0;
inWindow = valid & energy>=p.fomWindow(1) & energy<p.fomWindow(2);
g = psd(inWindow & ~isNeutron); n = psd(inWindow & isNeutron);
mu = [mean(g) mean(n)]; sigma = [std(g) std(n)];
fwhm = 2*sqrt(2*log(2))*sigma;
fom = abs(diff(mu))/sum(fwhm);
% 演示用监督阈值：用窗口内带标签数据确定，不是独立测试性能
threshold = mean(mu);
prediction = psd>threshold;
confusion = [sum(inWindow & ~isNeutron & ~prediction), sum(inWindow & ~isNeutron & prediction); ...
             sum(inWindow & isNeutron & ~prediction), sum(inWindow & isNeutron & prediction)];

%% 图 1：波形 -> 积分 -> 特征 -> 甄别
colors = [0.12 0.40 0.80; 0.85 0.25 0.12];
for j=1:2
    exampleWeights = [p.weightsGamma; p.weightsNeutron];
    examples(:,j) = makePulse(p.t,250,j==2,exampleWeights(j,:),0,p);
    examples(:,j) = examples(:,j)-mean(examples(p.t<-40,j));
end
fig = figure('Color','w','Position',[70 70 1200 800]);
tiledlayout(2,2,'TileSpacing','compact');
nexttile;
plot(p.t,examples(:,1),'Color',colors(1,:),'LineWidth',1.5); hold on;
plot(p.t,examples(:,2),'Color',colors(2,:),'LineWidth',1.5);
xline(p.gateTotal(1),'k:'); xline(p.gateTail(1),'k--'); xline(p.gateTotal(2),'k:');
xlim([-40 1250]); xlabel('Time (ns)'); ylabel('Voltage (mV)');
title('250 keVee example pulses'); legend('gamma','neutron','Total start','Tail start','End'); grid on;
nexttile;
scatter(energyMeasured(valid & ~isNeutron),psd(valid & ~isNeutron),5,colors(1,:),'filled'); hold on;
scatter(energyMeasured(valid & isNeutron),psd(valid & isNeutron),5,colors(2,:),'filled');
yline(threshold,'k--','Demo threshold'); xlabel('Integrated energy estimate (keVee)');
ylabel('PSD = Qtail / Qtotal'); title('Event features'); grid on;
nexttile;
edges = linspace(min([g;n])-0.02,max([g;n])+0.02,55);
histogram(g,edges,'Normalization','pdf','FaceColor',colors(1,:),'FaceAlpha',0.5); hold on;
histogram(n,edges,'Normalization','pdf','FaceColor',colors(2,:),'FaceAlpha',0.5);
x = linspace(edges(1),edges(end),400);
for j=1:2
    plot(x,exp(-0.5*((x-mu(j))/sigma(j)).^2)/(sigma(j)*sqrt(2*pi)), ...
        'Color',colors(j,:),'LineWidth',1.5);
end
xline(threshold,'k--'); xlabel('PSD'); ylabel('Probability density');
title(sprintf('%g-%g keVee: FOM = %.2f',p.fomWindow,fom)); grid on;
nexttile;
imagesc(confusion); axis image; colormap(gca,parula); colorbar;
xticks(1:2); yticks(1:2); xticklabels({'gamma','neutron'}); yticklabels({'gamma','neutron'});
xlabel('Predicted class'); ylabel('True class'); title('Same-sample demo classification');
for row=1:2
    for col=1:2
        text(col,row,num2str(confusion(row,col)),'HorizontalAlignment','center','Color','w','FontWeight','bold');
    end
end
sgtitle(sprintf('%s | %g MS/s | published decay times, assumed weights/readout',p.material,p.fsMHz));
exportgraphics(fig,fullfile(outDir,'psd_demo.png'),'Resolution',160);
savefig(fig,fullfile(outDir,'psd_demo.fig'));

%% 图 2：随能量变化的分离能力
centers = 125:50:575; fomByEnergy = nan(size(centers));
for j=1:numel(centers)
    sel = valid & abs(energy-centers(j))<25;
    a = psd(sel & ~isNeutron); b = psd(sel & isNeutron);
    fomByEnergy(j) = abs(mean(a)-mean(b))/(2*sqrt(2*log(2))*(std(a)+std(b)));
end
fig2 = figure('Color','w');
plot(centers,fomByEnergy,'o-','LineWidth',1.5); grid on;
xlabel('True energy bin center (keVee)'); ylabel('Gaussian-moment FOM');
title('Illustrative energy dependence; not detector validation');
exportgraphics(fig2,fullfile(outDir,'fom_vs_energy.png'),'Resolution',160);

%% 图 3：同一批波形的三种常规 PSD 方法对比
features = [psd meanTime fallTime];
methodNames = {'Tail charge ratio','Charge-weighted mean time','80%-20% fall time'};
featureUnits = {'Qtail / Qtotal','Mean time (ns)','Fall time (ns)'};
methodFOM = nan(3,1); methodThreshold = methodFOM; accuracy = methodFOM;
validGamma = zeros(3,1); validNeutron = validGamma;
methodPrediction = false(numel(energy),3);
methodConfusion = zeros(2,2,3); energyFOM = nan(numel(centers),3);
fig3 = figure('Color','w','Position',[20 40 1500 1050]);
tiledlayout(3,3,'TileSpacing','compact');
for m=1:3
    z = features(:,m);
    usable = qTotal>0 & isfinite(z);
    selected = usable & energy>=p.fomWindow(1) & energy<p.fomWindow(2);
    a = z(selected & ~isNeutron); b = z(selected & isNeutron);
    means = [mean(a) mean(b)]; deviations = [std(a) std(b)];
    methodFOM(m) = abs(diff(means))/(2*sqrt(2*log(2))*sum(deviations));
    methodThreshold(m) = mean(means);
    methodPrediction(:,m) = (z-methodThreshold(m))*sign(diff(means))>0;
    pred = methodPrediction(:,m);
    cm = [sum(selected & ~isNeutron & ~pred),sum(selected & ~isNeutron & pred); ...
          sum(selected & isNeutron & ~pred),sum(selected & isNeutron & pred)];
    methodConfusion(:,:,m) = cm;
    accuracy(m) = 100*trace(cm)/sum(cm(:));
    validGamma(m) = numel(a); validNeutron(m) = numel(b);
    nexttile(m);
    scatter(energyMeasured(usable & ~isNeutron),z(usable & ~isNeutron),4,colors(1,:),'filled'); hold on;
    scatter(energyMeasured(usable & isNeutron),z(usable & isNeutron),4,colors(2,:),'filled');
    yline(methodThreshold(m),'k--'); xlabel('Integrated energy (keVee)'); ylabel(featureUnits{m});
    title(methodNames{m}); grid on;
    nexttile(3+m);
    span = max([a;b])-min([a;b]);
    bins = linspace(min([a;b])-0.05*span,max([a;b])+0.05*span,50);
    histogram(a,bins,'Normalization','pdf','FaceColor',colors(1,:),'FaceAlpha',0.5); hold on;
    histogram(b,bins,'Normalization','pdf','FaceColor',colors(2,:),'FaceAlpha',0.5);
    xline(methodThreshold(m),'k--'); xlabel(featureUnits{m}); ylabel('Probability density');
    title(sprintf('225-275 keVee | FOM %.3f',methodFOM(m))); grid on;
    nexttile(6+m);
    imagesc(cm); axis image; colorbar;
    xticks(1:2); yticks(1:2); xticklabels({'gamma','neutron'}); yticklabels({'gamma','neutron'});
    xlabel('Predicted'); ylabel('True'); title(sprintf('Demo accuracy %.2f%% | valid %d',accuracy(m),sum(cm(:))));
    for row=1:2
        for col=1:2
            text(col,row,num2str(cm(row,col)),'HorizontalAlignment','center','Color','w','FontWeight','bold');
        end
    end
    for j=1:numel(centers)
        selectedBin = usable & abs(energy-centers(j))<25;
        aa = z(selectedBin & ~isNeutron); bb = z(selectedBin & isNeutron);
        energyFOM(j,m) = abs(mean(aa)-mean(bb))/(2*sqrt(2*log(2))*(std(aa)+std(bb)));
    end
end
sgtitle(sprintf('%s: three PSD methods | assumed weights/readout, uncalibrated',p.material));
exportgraphics(fig3,fullfile(outDir,'psd_methods_comparison.png'),'Resolution',150);
savefig(fig3,fullfile(outDir,'psd_methods_comparison.fig'));
figure(fig2); cla;
plot(centers,energyFOM,'o-','LineWidth',1.5); grid on;
xlabel('True energy bin center (keVee)'); ylabel('Gaussian-moment FOM');
legend(methodNames,'Location','best'); title('Three-method energy dependence (illustrative)');
exportgraphics(fig2,fullfile(outDir,'fom_vs_energy.png'),'Resolution',160);
comparison = table(string(methodNames(:)),methodFOM,methodThreshold,accuracy,validGamma,validNeutron, ...
    'VariableNames',{'Method','FOM','Threshold','DemoAccuracyPercent','ValidGamma','ValidNeutron'});
writetable(comparison,fullfile(outDir,'method_comparison.csv'));
disp(comparison);
events = table(energy,energyMeasured,isNeutron,qTotal,qTail,psd,prediction,meanTime,fallTime);
writetable(events,fullfile(outDir,'events.csv'));
save(fullfile(outDir,'results.mat'),'p','events','fom','mu','sigma','confusion','fomByEnergy','centers', ...
    'comparison','features','methodPrediction','methodConfusion','energyFOM');
fprintf('250 keVee window [%g,%g): gamma=%d, neutron=%d\n',p.fomWindow,numel(g),numel(n));
fprintf('FOM = %.3f; threshold = %.4f\n',fom,threshold);
fprintf('Demo window accuracy = %.2f%% (same data used to set threshold)\n',100*trace(confusion)/sum(confusion(:)));
fprintf('Outputs: %s\n',outDir);

%% 面积归一化双衰减模型，再叠加电子学、噪声与量化
function value = pulseFallTime(t,w,p)
    % 峰值在前 80 ns 搜索；下降沿首次越过目标幅度，线性插值。
    % 不额外平滑，以便三种方法使用同样的输入；噪声会影响首次交越。
    indices = find(t>=p.gateTotal(1) & t<80);
    [peak,localIndex] = max(w(indices));
    peakIndex = indices(localIndex);
    value = NaN;
    if peak<=0, return; end
    crossing = nan(1,2);
    endIndex = find(t<p.gateTotal(2),1,'last');
    for level=1:2
        target = peak*p.fallLevels(level);
        relativeIndex = find(w(peakIndex+1:endIndex)<=target,1,'first');
        if isempty(relativeIndex), return; end
        index = peakIndex+relativeIndex;
        crossing(level) = t(index-1)+(target-w(index-1))*(t(index)-t(index-1))/(w(index)-w(index-1));
    end
    value = crossing(2)-crossing(1);
end

function w = makePulse(t,E,isNeutron,weights,t0,p)
    u = t-t0; positive = u>=0; u = max(u,0);
    tau = p.tauGamma;
    if isNeutron, tau = p.tauNeutron; end
    % 每个核的连续时间积分为 1，weights 因而表示积分光量比例。
    components = (exp(-u./tau)-exp(-u/p.tauRise))./(tau-p.tauRise);
    w = p.gain*E*(components*weights(:)).*positive;
    if p.tauElectronics>0
        alpha = exp(-(1000/p.fsMHz)/p.tauElectronics);
        w = filter(1-alpha,[1 -alpha],w);
    end
    w = w + p.noiseRMS*randn(size(w)) + 0.5*randn;
    step = p.adcFullScale/2^p.adcBits;
    w = min(max(round(w/step),-2^(p.adcBits-1)),2^(p.adcBits-1)-1)*step;
end
