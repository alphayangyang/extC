! bench/fortran/stencil.f90 —— 2D 5 点 Jacobi 模板（f64，flat 行主序）。
!
! 与 stencil.extc / stencil.c 同形状、同参数、同 stdout 字节；口径见 SPEC.md。
! 用法：stencil [n] [iters]，缺省 64 10。输出：cs=<整数> n=<n>
program stencil
  implicit none
  integer(8) :: n, iters, cells, i, j, r, c, it, cs
  real(8), pointer :: a(:), b(:), t(:)
  real(8) :: csd
  character(len=64) :: arg

  n = 64_8
  iters = 10_8
  if (command_argument_count() >= 1) then
     call get_command_argument(1, arg)
     read (arg, *) n
  end if
  if (command_argument_count() >= 2) then
     call get_command_argument(2, arg)
     read (arg, *) iters
  end if

  cells = n * n
  allocate(a(cells), b(cells))
  b = 0.0d0  ! 两块都从零开始；之后只交换指针，不做逐元素拷贝

  do i = 0, n - 1
     do j = 0, n - 1
        a(i * n + j + 1) = dble(mod(i * 7_8 + j * 13_8, 17_8)) / 17.0d0
     end do
  end do

  do it = 1, iters
     do r = 1, n - 2
        do c = 1, n - 2
           b(r * n + c + 1) = (a((r - 1) * n + c + 1) + a((r + 1) * n + c + 1) &
                + a(r * n + c) + a(r * n + c + 2)) * 0.25d0
        end do
     end do
     t => a
     a => b
     b => t
  end do

  csd = 0.0d0
  do i = 0, n - 1
     do j = 0, n - 1
        csd = csd + a(i * n + j + 1)
     end do
  end do
  cs = int(csd * 1000000.0d0, 8)

  write (*, '(A,I0,A,I0)') 'cs=', cs, ' n=', n
end program stencil
