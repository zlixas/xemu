!!TS1.0
texture_2d();
nop();
nop();
nop();

!!RC1.0
{
  rgb {
    spare0 = tex0 * col0;
  }
  alpha {
    spare0 = tex0.a * col0.a;
  }
}
out.rgb = spare0;
out.a = spare0.a;
