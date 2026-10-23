function parts = pulse_parts(c,taus,weights,rise,peak,support,dt)
% 每采样区间32个中点平均；三分量共同乘上升项、收尾窗并统一峰值归一。
sub = ((0:support-1)' + ((0:31)+.5)/32)*dt;
t = (0:support-1)'*dt;
tail = ones(support,1); mask = t>c.taper;
tail(mask) = .5*(1+cos(pi*(t(mask)-c.taper)/(t(end)-c.taper)));
tail(end)=0;
parts=zeros(support,3);
for j=1:3
    parts(:,j)=mean(weights(j)*(-expm1(-sub/rise)).*exp(-sub/taus(j)),2).*tail;
end
maximum=max(sum(parts,2));
assert(maximum>0,'采样波形为零，请调整时间常数或采样率。');
parts=parts*(peak/maximum);
end
