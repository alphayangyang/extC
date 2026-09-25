(function(){var q=document.getElementById('q');if(!q)return;
var items=[].slice.call(document.querySelectorAll('.side li'));
q.addEventListener('input',function(){var v=q.value.trim().toLowerCase();
items.forEach(function(li){li.style.display=!v||li.textContent.toLowerCase().indexOf(v)>=0?'':'none'});});
document.addEventListener('keydown',function(e){if(e.key==='/'&&document.activeElement!==q){e.preventDefault();q.focus();}});})();
