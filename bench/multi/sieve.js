const n = 10000000, comp = new Uint8Array(n+1); let cnt = 0;
for (let i = 2; i <= n; i++) if (!comp[i]) { cnt++; for (let j = i*i; j <= n; j += i) comp[j] = 1; }
console.log("pi =", cnt);
