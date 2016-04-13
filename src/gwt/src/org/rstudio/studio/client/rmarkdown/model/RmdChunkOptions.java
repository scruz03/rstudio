/*
 * RmdChunkOptions.java
 *
 * Copyright (C) 2009-16 by RStudio, Inc.
 *
 * Unless you have received this program directly from RStudio pursuant
 * to the terms of a commercial license agreement with RStudio, then
 * this program is licensed to you under the terms of version 3 of the
 * GNU Affero General Public License. This program is distributed WITHOUT
 * ANY EXPRESS OR IMPLIED WARRANTY, INCLUDING THOSE OF NON-INFRINGEMENT,
 * MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE. Please refer to the
 * AGPL (http://www.gnu.org/licenses/agpl-3.0.txt) for more details.
 *
 */
package org.rstudio.studio.client.rmarkdown.model;

import com.google.gwt.core.client.JavaScriptObject;

public class RmdChunkOptions extends JavaScriptObject
{
   protected RmdChunkOptions()
   {
   }
   
   public final native boolean eval() /*-{
      if (typeof(this.eval) !== "undefined")
        return !!this.eval;
      return true;
   }-*/;

   public final native boolean include() /*-{
      if (typeof(this.include) !== "undefined")
        return !!this.include;
      return true;
   }-*/;
}