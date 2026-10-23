function figs = plot_pulses(c,r)
colors=[.16 .46 .76;.80 .32 .12];
figs(1)=figure('Color','w','Name','各类实际首脉冲与三分量','Position',[80 80 1100 650]);
names=fieldnames(r.representatives);
for j=1:numel(names)
    ax=subplot(numel(names),1,j); parts=r.representatives.(names{j});
    % 前补零仅用于显示，不改变导出采样或事件时刻。
    t=[0;500;(0:r.support-1)'*r.dt+500+r.dt/2];
    plot(ax,t,[zeros(2,3);parts],'LineWidth',1); hold(ax,'on');
    plot(ax,t,[0;0;sum(parts,2)],'k','LineWidth',1.5);
    title(ax,[names{j} ' : first actual event (baseline subtracted)']);
    xlabel(ax,'time (ns)'); ylabel(ax,'amplitude');
    legend(ax,{'fast','middle','slow','sum'}); grid(ax,'on');
end
figs(2)=figure('Color','w','Name','连续混排序列与混排逻辑','Position',[100 100 1200 730]);
n=min(c.points,c.preview_events*r.slot); time=(0:n-1)'*r.dt/1000;
ax=subplot(3,1,[1 2]); hold(ax,'on');
% 完整连续曲线保留所有相邻采样连线，包括类别切换处。
plot(ax,time,r.noisy(1:n),'Color',[.55 .55 .55],'LineWidth',.7,'DisplayName','continuous + noise');
plot(ax,time,r.clean(1:n),'k--','LineWidth',1,'DisplayName','continuous clean');
% 每段包含下一事件的首点，确保类别切换处也有连接。
for i=1:min(height(r.events),c.preview_events)
    idx=(i-1)*r.slot+1:min(i*r.slot+1,n);
    if strcmp(c.mode,'custom'), color=[.25 .25 .25]; else, color=colors(r.kinds(i)+1,:); end
    plot(ax,time(idx),r.noisy(idx),'Color',color,'LineWidth',.8,'HandleVisibility','off');
end
xlim(ax,[time(1) time(end)]); xlabel(ax,'time (us)'); ylabel(ax,'amplitude');
title(ax,'(b) Continuous sampled sequence (color = particle type)'); grid(ax,'on'); legend(ax,'show');
ax=subplot(3,1,3); hold(ax,'on');
for i=1:min(height(r.events),c.preview_events)
    left=(i-1)*r.slot*r.dt/1000; right=min(i*r.slot,n)*r.dt/1000;
    if strcmp(c.mode,'custom'), color=[.25 .25 .25]; else, color=colors(r.kinds(i)+1,:); end
    patch(ax,[left right right left],[0 0 1 1],color,'FaceAlpha',.20,'EdgeColor',color);
    text(ax,(left+right)/2,.5,sprintf('%d: %s',i,r.events.particle{i}),'HorizontalAlignment','center','FontSize',8);
end
xlim(ax,[time(1) time(end)]); ylim(ax,[0 1]); yticks(ax,[]); xlabel(ax,'time (us)');
title(ax,sprintf('Balanced labels -> seeded shuffle -> event jitter -> pulses + noise; gamma=%d, neutron=%d',sum(r.kinds==0 & ~strcmp(c.mode,'custom')),sum(r.kinds==1)));
end
