﻿using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Drawing;
using System.Data;
using System.Linq;
using System.Text;
using System.Threading.Tasks;
using System.Windows.Forms;

namespace Pyramid
{
    public partial class FXCResultsPanel : UserControl
    {
        private static void HexBumpBlob( StringBuilder str, byte[] bytes )
        {
            int nBytes = bytes.Length;

            if (nBytes % 4 == 0)
            {
                for (int i = 0; i < nBytes; i += 4)
                {
                    int n = bytes[i] |
                             bytes[i + 1] << 8 |
                             bytes[i + 2] << 16 |
                             bytes[i + 3] << 24;

                    char c0 = Convert.ToChar(bytes[i]);
                    char c1 = Convert.ToChar(bytes[i+1]);
                    char c2 = Convert.ToChar(bytes[i+2]);
                    char c3 = Convert.ToChar(bytes[i+3]);
                    c0 = (c0 < 0x20 || c0 > 127) ? '.' : c0;
                    c1 = (c1 < 0x20 || c1 > 127) ? '.' : c1;
                    c2 = (c2 < 0x20 || c2 > 127) ? '.' : c2;
                    c3 = (c3 < 0x20 || c3 > 127) ? '.' : c3;
                    str.AppendFormat("{0:X8}  '{1} {2} {3} {4}' ", n, c0,c1,c2,c3 );
                    str.AppendLine();
                }
            }
            else
            {
                for( int i=0; i<nBytes; i++ )
                {
                    if (i % 8 == 0)
                        str.AppendLine();
                    str.AppendFormat("{0:X2} ", bytes[i]);
                }
            }
            str.AppendLine();
        }

        public FXCResultsPanel( string error, IDXShaderBlob blob )
        {
            InitializeComponent();
            txtMessages.Text = error;

            if (blob == null)
            {
                tabControl1.TabPages.Remove(hexPage);
                tabControl1.TabPages.Remove(asmPage);
            }
            else
            {
                txtASM.Text = blob.Disassemble();

                // generate a blob dump
                try
                {
                    IDXShaderBlob strip = blob.Strip();
                    IDXShaderBlob sig   = strip.GetSignatureBlob();

                    byte[] rawBytes = blob.ReadBytes();
                    byte[] stripBytes = strip.ReadBytes();
                    byte[] sigBytes = sig.ReadBytes();

                    StringBuilder str = new StringBuilder();
                    str.AppendFormat("Blob size is {0} ({1:X}) bytes", rawBytes.Length, rawBytes.Length);
                    str.AppendLine();
                    HexBumpBlob(str, rawBytes);

                    str.AppendFormat("Stripped blob size is {0} ({1:x}) bytes", stripBytes.Length, stripBytes.Length);
                    str.AppendLine();
                    HexBumpBlob(str, stripBytes);

                    str.AppendFormat("Signature blob size is {0} ({1:x}) bytes", sigBytes.Length, sigBytes.Length);
                    str.AppendLine();
                    HexBumpBlob(str, sigBytes);

                    txtHex.Text = str.ToString();
                }
                catch( Exception ex )
                {
                    txtHex.Text = String.Format("EXCEPTION while generating hex dump: {0}", ex.Message);
                }
                
            }
        }
    }
}
